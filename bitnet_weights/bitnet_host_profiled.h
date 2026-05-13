// =============================================================================
// bitnet_host_profiled.h
// =============================================================================
//
// Drop-in patch for BitNetHostInference private methods.
//
// USAGE:
//   In bitnet_inference.h, inside class BitNetHostInference, replace the
//   private section with:
//
//       #if defined(BITNET_PROFILE)
//       #  include "bitnet_host_profiled.h"
//       #else
//       // ... original private methods ...
//       #endif
//
//   Build with:  -DBITNET_PROFILE -O3 -march=native -mavx2
//
// OUTPUT FORMAT (per token, to stderr so it doesn't corrupt stdout token stream):
//
//   [PROF token=42 total=187ms]
//   [PROF]   embedding          :   0.03 ms
//   [PROF]   layer_avg (x24)    :   6.89 ms   (attn=3.21  mlp=3.41  norm=0.27)
//   [PROF]     linear_q         :   0.81 ms avg
//   [PROF]     linear_k         :   0.81 ms avg
//   [PROF]     linear_v         :   0.81 ms avg
//   [PROF]     rope              :   0.04 ms avg
//   [PROF]     kv_cache_quant   :   0.03 ms avg
//   [PROF]     attn_scores      :   0.12 ms avg  (seq_len=N)
//   [PROF]     attn_softmax     :   0.03 ms avg
//   [PROF]     attn_sub_norm    :   0.02 ms avg
//   [PROF]     linear_o         :   0.81 ms avg
//   [PROF]     linear_gate      :   1.10 ms avg
//   [PROF]     linear_up        :   1.10 ms avg
//   [PROF]     relu2_act        :   0.01 ms avg
//   [PROF]     ffn_sub_norm     :   0.02 ms avg
//   [PROF]     linear_down      :   0.61 ms avg
//   [PROF]   final_norm         :   0.11 ms
//   [PROF]   argmax_logits      :  20.34 ms
//   [PROF]   weight_prefetch    :   3.21 ms (wait stalls)
//   [PROF]   linear_quantize_A8 :  18.41 ms total (activation quant)
//   [PROF]   linear_pool_exec   : 140.22 ms total (actual int8 matmul)
//   [PROF]   thread_pool_size   : 12 workers
//   [PROF]   tokens_per_sec     :   5.35
//
// =============================================================================

#include "bitnet_profiler.h"

// Bring the profiler namespace in locally
using PNanos = sandbox::bitnet::Nanos;
#define P_NOW()   sandbox::bitnet::nowNs()
#define P_REC(name, ns)  sandbox::bitnet::globalProfiler().record(name, ns)

private:

    // -------------------------------------------------------------------------
    // Per-token accumulated stage timers (reset each evalToken call)
    // -------------------------------------------------------------------------
    struct LayerAccum {
        PNanos linear_q_ns      = 0;
        PNanos linear_k_ns      = 0;
        PNanos linear_v_ns      = 0;
        PNanos rope_ns          = 0;
        PNanos kv_quant_ns      = 0;
        PNanos attn_scores_ns   = 0;
        PNanos attn_softmax_ns  = 0;
        PNanos attn_sub_norm_ns = 0;
        PNanos linear_o_ns      = 0;
        PNanos linear_gate_ns   = 0;
        PNanos linear_up_ns     = 0;
        PNanos relu2_ns         = 0;
        PNanos ffn_sub_norm_ns  = 0;
        PNanos linear_down_ns   = 0;
        PNanos rmsnorm_pre_ns   = 0;
        PNanos rmsnorm_post_ns  = 0;
        PNanos prefetch_wait_ns = 0;
        int    layer_count      = 0;
    };

    struct TokenAccum {
        PNanos embed_ns         = 0;
        PNanos final_norm_ns    = 0;
        PNanos argmax_ns        = 0;
        PNanos aq_total_ns      = 0;   // activation quantization (inside linearPacked)
        PNanos pool_total_ns    = 0;   // ThreadPool::execute (actual matmul)
        PNanos token_wall_ns    = 0;
        int    linear_calls     = 0;
        LayerAccum layers;
    };

    mutable TokenAccum tok_;  // populated during evalToken

    // -------------------------------------------------------------------------
    // Instrumented evalToken
    // -------------------------------------------------------------------------
    BitNetHostForwardResult evalToken(int tokenId, bool computeLogits) {
        BitNetHostForwardResult result;
        if (!ready_) { result.error = "BitNetHostInference not initialized"; return result; }
        if (kv_len_ >= cfg.max_position_embeddings) { result.error = "KV cache is full"; return result; }

        tok_ = TokenAccum{};
        sandbox::bitnet::globalProfiler().reset();

        const auto t0 = P_NOW();
        std::string error;

        // --- Embedding ---
        auto _t = P_NOW();
        std::vector<float> x;
        if (!embeddingRow(tokenId, x, error)) { result.error = error; return result; }
        tok_.embed_ns = P_NOW() - _t;

        const int position = kv_len_;
        prefetchLayer(0);

        for (int layer = 0; layer < cfg.num_layers; ++layer) {
            auto _layer_start = P_NOW();

            if (layer + 1 < cfg.num_layers) prefetchLayer(layer + 1);

            auto _pw = P_NOW();
            ensureLayerLoaded(layer);
            tok_.layers.prefetch_wait_ns += P_NOW() - _pw;

            const std::string pfx = "model.layers." + std::to_string(layer) + ".";

            // Pre-attention RMSNorm
            if (residual_scratch_.size() < x.size()) residual_scratch_.resize(x.size());
            std::copy(x.begin(), x.end(), residual_scratch_.begin());
            if (norm_scratch_.size() < x.size()) norm_scratch_.resize(x.size());

            auto _rn1 = P_NOW();
            if (!rmsNorm(x, tensorFloat(pfx + "input_layernorm.weight", error), norm_scratch_, error)) {
                result.error = error; return result;
            }
            tok_.layers.rmsnorm_pre_ns += P_NOW() - _rn1;

            // Attention
            if (attn_scratch_.size() < x.size()) attn_scratch_.resize(x.size());
            {
                auto _attn = P_NOW();
                if (!attentionLayerInstrumented(layer, pfx, norm_scratch_, position, attn_scratch_, error)) {
                    result.error = error; return result;
                }
                (void)_attn; // individual sub-timings stored in tok_.layers.*
            }
            addInto(residual_scratch_, attn_scratch_, x);

            // Post-attention RMSNorm
            std::copy(x.begin(), x.end(), residual_scratch_.begin());
            auto _rn2 = P_NOW();
            if (!rmsNorm(x, tensorFloat(pfx + "post_attention_layernorm.weight", error), norm_scratch_, error)) {
                result.error = error; return result;
            }
            tok_.layers.rmsnorm_post_ns += P_NOW() - _rn2;

            // MLP
            if (mlp_scratch_.size() < x.size()) mlp_scratch_.resize(x.size());
            if (!mlpLayerInstrumented(pfx, norm_scratch_, mlp_scratch_, error)) {
                result.error = error; return result;
            }
            addInto(residual_scratch_, mlp_scratch_, x);

            // Safety clamp
            for (float& v : x) {
                if (!std::isfinite(v)) v = 0.0f;
                else if (v >  16384.0f) v =  16384.0f;
                else if (v < -16384.0f) v = -16384.0f;
            }

            tok_.layers.layer_count++;
            P_REC("layer_wall", P_NOW() - _layer_start);
        }

        ++kv_len_;

        if (computeLogits) {
            auto _fn = P_NOW();
            std::vector<float> norm;
            if (!rmsNorm(x, tensorFloat("model.norm.weight", error), norm, error)) {
                result.error = error; return result;
            }
            tok_.final_norm_ns = P_NOW() - _fn;

            auto _ax = P_NOW();
            if (!argmaxLogits(norm, result.greedy_token, result.greedy_logit, &result.logits)) {
                result.error = "Argmax failed"; return result;
            }
            tok_.argmax_ns = P_NOW() - _ax;
        }

        tok_.token_wall_ns = P_NOW() - t0;
        result.elapsed_ms  = tok_.token_wall_ns / 1'000'000;
        result.ok = true;

        profReport(std::cerr);
        return result;
    }

    // -------------------------------------------------------------------------
    // Instrumented attentionLayer
    // -------------------------------------------------------------------------
    bool attentionLayerInstrumented(
        int layer,
        const std::string& pfx,
        const std::vector<float>& norm,
        int position,
        std::vector<float>& out,
        std::string& error)
    {
        std::vector<float> q, k, v;

        auto _lq = P_NOW();
        if (!linearPacked(pfx + "self_attn.q_proj.weight", cfg.hidden_size, cfg.hidden_size, norm, q, error)) return false;
        tok_.layers.linear_q_ns += P_NOW() - _lq;

        auto _lk = P_NOW();
        if (!linearPacked(pfx + "self_attn.k_proj.weight", kv_dim_, cfg.hidden_size, norm, k, error)) return false;
        tok_.layers.linear_k_ns += P_NOW() - _lk;

        auto _lv = P_NOW();
        if (!linearPacked(pfx + "self_attn.v_proj.weight", kv_dim_, cfg.hidden_size, norm, v, error)) return false;
        tok_.layers.linear_v_ns += P_NOW() - _lv;

        auto _rope = P_NOW();
        applyRoPE(q, k, position);
        tok_.layers.rope_ns += P_NOW() - _rope;

        // KV cache quantization
        auto _kvcq = P_NOW();
        LayerKV& cache = kv_[static_cast<std::size_t>(layer)];
        for (int h = 0; h < cfg.num_kv_heads; ++h) {
            const float* kHead = k.data() + h * cfg.head_dim;
            const float* vHead = v.data() + h * cfg.head_dim;
            float k_max = 1e-9f, v_max = 1e-9f;
            for (int d = 0; d < cfg.head_dim; ++d) {
                k_max = std::max(k_max, std::abs(kHead[d]));
                v_max = std::max(v_max, std::abs(vHead[d]));
            }
            const float k_gamma = k_max / 127.0f;
            const float v_gamma = v_max / 127.0f;
            cache.k_scales[position * cfg.num_kv_heads + h] = k_gamma;
            cache.v_scales[position * cfg.num_kv_heads + h] = v_gamma;
            int8_t* kSlot = cache.keys.data()   + position * kv_dim_ + h * cfg.head_dim;
            int8_t* vSlot = cache.values.data() + position * kv_dim_ + h * cfg.head_dim;
            const float k_inv = 1.0f / k_gamma;
            const float v_inv = 1.0f / v_gamma;
            for (int d = 0; d < cfg.head_dim; ++d) {
                kSlot[d] = static_cast<int8_t>(std::round(std::clamp(kHead[d] * k_inv, -127.0f, 127.0f)));
                vSlot[d] = static_cast<int8_t>(std::round(std::clamp(vHead[d] * v_inv, -127.0f, 127.0f)));
            }
        }
        tok_.layers.kv_quant_ns += P_NOW() - _kvcq;

        // Attention scores + context aggregation
        std::vector<float> attn(static_cast<std::size_t>(cfg.hidden_size), 0.0f);
        const int   groups = cfg.num_heads / cfg.num_kv_heads;
        const float scale  = 1.0f / std::sqrt(static_cast<float>(cfg.head_dim));
        std::vector<float> scores(static_cast<std::size_t>(position + 1));

        auto _scores = P_NOW();
        for (int head = 0; head < cfg.num_heads; ++head) {
            const int    kvHead = head / groups;
            const float* qHead  = q.data() + static_cast<std::size_t>(head) * cfg.head_dim;

            float maxScore = -std::numeric_limits<float>::infinity();
            for (int t = 0; t <= position; ++t) {
                const int8_t* kHead2   = cache.keys.data()
                    + static_cast<std::size_t>(t) * kv_dim_
                    + static_cast<std::size_t>(kvHead) * cfg.head_dim;
                const float k_scale = cache.k_scales[t * cfg.num_kv_heads + kvHead];
                float dot = 0.0f;
#ifdef __AVX2__
                __m256 vacc = _mm256_setzero_ps();
                for (int d = 0; d <= cfg.head_dim - 8; d += 8) {
                    __m128i vraw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(kHead2 + d));
                    __m256i v32  = _mm256_cvtepi8_epi32(vraw);
                    __m256  vf   = _mm256_cvtepi32_ps(v32);
                    __m256  vq   = _mm256_loadu_ps(qHead + d);
                    vacc = _mm256_add_ps(vacc, _mm256_mul_ps(vf, vq));
                }
                float tmp[8]; _mm256_storeu_ps(tmp, vacc);
                for (int i = 0; i < 8; ++i) dot += tmp[i];
#else
                for (int d = 0; d < cfg.head_dim; ++d) dot += qHead[d] * static_cast<float>(kHead2[d]);
#endif
                dot *= k_scale;
                scores[static_cast<std::size_t>(t)] = dot * scale;
                maxScore = std::max(maxScore, scores[static_cast<std::size_t>(t)]);
            }

            // Softmax
            float denom = 0.0f;
            for (int t = 0; t <= position; ++t) {
                float e = std::exp(scores[static_cast<std::size_t>(t)] - maxScore);
                scores[static_cast<std::size_t>(t)] = e;
                denom += e;
            }
            if (denom == 0.0f || !std::isfinite(denom)) { error = "Attention softmax failed"; return false; }

            // Context aggregation
            float* outHead = attn.data() + static_cast<std::size_t>(head) * cfg.head_dim;
            for (int t = 0; t <= position; ++t) {
                const int8_t* vHead = cache.values.data()
                    + static_cast<std::size_t>(t) * kv_dim_
                    + static_cast<std::size_t>(kvHead) * cfg.head_dim;
                const float v_scale = cache.v_scales[t * cfg.num_kv_heads + kvHead];
                const float prob    = (scores[static_cast<std::size_t>(t)] / denom) * v_scale;
#ifdef __AVX2__
                __m256 vprob = _mm256_set1_ps(prob);
                for (int d = 0; d <= cfg.head_dim - 8; d += 8) {
                    __m128i vraw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(vHead + d));
                    __m256i v32  = _mm256_cvtepi8_epi32(vraw);
                    __m256  vf   = _mm256_cvtepi32_ps(v32);
                    __m256  vo   = _mm256_loadu_ps(outHead + d);
                    vo = _mm256_add_ps(vo, _mm256_mul_ps(vf, vprob));
                    _mm256_storeu_ps(outHead + d, vo);
                }
#else
                for (int d = 0; d < cfg.head_dim; ++d) outHead[d] += prob * static_cast<float>(vHead[d]);
#endif
            }
        }
        tok_.layers.attn_scores_ns += P_NOW() - _scores;

        // Sub-norm + output projection
        std::vector<float> attnNorm;
        auto _asn = P_NOW();
        if (!rmsNorm(attn, tensorFloat(pfx + "self_attn.attn_sub_norm.weight", error), attnNorm, error)) return false;
        tok_.layers.attn_sub_norm_ns += P_NOW() - _asn;

        auto _lo = P_NOW();
        bool ok = linearPacked(pfx + "self_attn.o_proj.weight", cfg.hidden_size, cfg.hidden_size, attnNorm, out, error);
        tok_.layers.linear_o_ns += P_NOW() - _lo;
        return ok;
    }

    // -------------------------------------------------------------------------
    // Instrumented mlpLayer
    // -------------------------------------------------------------------------
    bool mlpLayerInstrumented(
        const std::string& pfx,
        const std::vector<float>& norm,
        std::vector<float>& out,
        std::string& error)
    {
        std::vector<float> gate, up;

        auto _lg = P_NOW();
        if (!linearPacked(pfx + "mlp.gate_proj.weight", cfg.intermediate_size, cfg.hidden_size, norm, gate, error)) return false;
        tok_.layers.linear_gate_ns += P_NOW() - _lg;

        auto _lu = P_NOW();
        if (!linearPacked(pfx + "mlp.up_proj.weight", cfg.intermediate_size, cfg.hidden_size, norm, up, error)) return false;
        tok_.layers.linear_up_ns += P_NOW() - _lu;

        // relu2 activation
        auto _act = P_NOW();
        std::vector<float> activated(static_cast<std::size_t>(cfg.intermediate_size));
        for (int i = 0; i < cfg.intermediate_size; ++i)
            activated[static_cast<std::size_t>(i)] = relu2(gate[static_cast<std::size_t>(i)])
                                                    * up[static_cast<std::size_t>(i)];
        tok_.layers.relu2_ns += P_NOW() - _act;

        // FFN sub-norm
        std::vector<float> ffnNorm;
        auto _fsn = P_NOW();
        if (!rmsNorm(activated, tensorFloat(pfx + "mlp.ffn_sub_norm.weight", error), ffnNorm, error)) return false;
        tok_.layers.ffn_sub_norm_ns += P_NOW() - _fsn;

        // Down projection
        auto _ld = P_NOW();
        bool ok = linearPacked(pfx + "mlp.down_proj.weight", cfg.hidden_size, cfg.intermediate_size, ffnNorm, out, error);
        tok_.layers.linear_down_ns += P_NOW() - _ld;
        return ok;
    }

    // -------------------------------------------------------------------------
    // Instrumented linearPacked  (replaces original)
    // -------------------------------------------------------------------------
    bool linearPacked(
        const std::string& name,
        int rows,
        int cols,
        const std::vector<float>& x,
        std::vector<float>& out,
        std::string& error)
    {
        if (static_cast<int>(x.size()) != cols) { error = "Linear input shape mismatch for " + name; return false; }
        const auto layer = loader.getLayer(name);
        if (layer.name.empty()) { error = "Missing packed layer: " + name; return false; }
        if (layer.count < rows * cols) { error = "Packed layer too small: " + name; return false; }
        const auto* unpacked = loader.unpackedL50(name);
        if (!unpacked) { error = "Could not load unpacked L50 layer: " + name; return false; }

        out.assign(static_cast<std::size_t>(rows), 0.0f);
        const float w_scale = static_cast<float>(loader.getScale(name));
        tok_.linear_calls++;

        // ---- Activation quantization (A8) ----
        auto _aq = P_NOW();
        float x_max = 1e-9f;
#ifdef __AVX2__
        __m256 vmax = _mm256_set1_ps(1e-9f);
        int jj = 0;
        for (; jj <= cols - 8; jj += 8) {
            __m256 vx   = _mm256_loadu_ps(x.data() + jj);
            __m256 vabs = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), vx);
            vmax = _mm256_max_ps(vmax, vabs);
        }
        float tmp_max[8]; _mm256_storeu_ps(tmp_max, vmax);
        for (int i = 0; i < 8; ++i) x_max = std::max(x_max, tmp_max[i]);
        for (; jj < cols; ++jj) x_max = std::max(x_max, std::abs(x[static_cast<std::size_t>(jj)]));
#else
        for (float v : x) x_max = std::max(x_max, std::abs(v));
#endif
        const float x_gamma = x_max / 127.0f;
        const float x_inv   = 1.0f  / x_gamma;
        if (x_q_scratch_.size() < static_cast<std::size_t>(cols))
            x_q_scratch_.resize(static_cast<std::size_t>(cols));

#ifdef __AVX2__
        __m256 vinv = _mm256_set1_ps(x_inv);
        int jq = 0;
        for (; jq <= cols - 8; jq += 8) {
            __m256  vx     = _mm256_loadu_ps(x.data() + jq);
            __m256  vsc    = _mm256_mul_ps(vx, vinv);
            __m256i v32    = _mm256_cvtps_epi32(vsc);
            int32_t t32[8]; _mm256_storeu_si256(reinterpret_cast<__m256i*>(t32), v32);
            for (int i = 0; i < 8; ++i)
                x_q_scratch_[static_cast<std::size_t>(jq + i)] =
                    static_cast<int8_t>(std::clamp(t32[i], -127, 127));
        }
        for (; jq < cols; ++jq)
            x_q_scratch_[static_cast<std::size_t>(jq)] =
                static_cast<int8_t>(std::round(std::clamp(x[static_cast<std::size_t>(jq)] * x_inv, -127.0f, 127.0f)));
#else
        for (int j = 0; j < cols; ++j)
            x_q_scratch_[static_cast<std::size_t>(j)] =
                static_cast<int8_t>(std::round(std::clamp(x[static_cast<std::size_t>(j)] * x_inv, -127.0f, 127.0f)));
#endif
        tok_.aq_total_ns += P_NOW() - _aq;

        // ---- Actual int8 matmul (ThreadPool) ----
        const float combined_scale = w_scale * x_gamma;
        const int   num_workers    = static_cast<int>(pool_.size());
        const int   chunk          = (rows + num_workers - 1) / num_workers;

        auto _pe = P_NOW();
        pool_.execute(num_workers, [&](int worker) {
            const int rowBegin = worker * chunk;
            const int rowEnd   = std::min(rows, rowBegin + chunk);
            for (int row = rowBegin; row < rowEnd; ++row) {
                const int8_t* w_row = unpacked->data() + static_cast<std::size_t>(row) * cols;
                int32_t acc = 0;
                int j = 0;
#ifdef __AVX2__
                __m256i vsum16_0 = _mm256_setzero_si256();
                __m256i vsum16_1 = _mm256_setzero_si256();
                for (; j <= cols - 32; j += 32) {
                    __m256i vx  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x_q_scratch_.data() + j));
                    __m256i vw  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w_row + j));
                    __m256i vs  = _mm256_sign_epi8(vx, vw);
                    vsum16_0 = _mm256_add_epi16(vsum16_0, _mm256_cvtepi8_epi16(_mm256_extracti128_si256(vs, 0)));
                    vsum16_1 = _mm256_add_epi16(vsum16_1, _mm256_cvtepi8_epi16(_mm256_extracti128_si256(vs, 1)));
                }
                __m256i vsum32 = _mm256_add_epi32(
                    _mm256_add_epi32(_mm256_cvtepi16_epi32(_mm256_extracti128_si256(vsum16_0, 0)),
                                     _mm256_cvtepi16_epi32(_mm256_extracti128_si256(vsum16_0, 1))),
                    _mm256_add_epi32(_mm256_cvtepi16_epi32(_mm256_extracti128_si256(vsum16_1, 0)),
                                     _mm256_cvtepi16_epi32(_mm256_extracti128_si256(vsum16_1, 1))));
                int32_t tmp[8]; _mm256_storeu_si256(reinterpret_cast<__m256i*>(tmp), vsum32);
                for (int i = 0; i < 8; ++i) acc += tmp[i];
#endif
                for (; j < cols; ++j) {
                    int8_t w = w_row[j];
                    if (w ==  1) acc += x_q_scratch_[static_cast<std::size_t>(j)];
                    else if (w == -1) acc -= x_q_scratch_[static_cast<std::size_t>(j)];
                }
                out[static_cast<std::size_t>(row)] = static_cast<float>(acc) * combined_scale;
            }
        });
        tok_.pool_total_ns += P_NOW() - _pe;

        return true;
    }

    // -------------------------------------------------------------------------
    // profReport — called after each token, prints to the given stream
    // -------------------------------------------------------------------------
    void profReport(std::ostream& err) const {
        const double wall_ms  = tok_.token_wall_ns / 1e6;
        const double tps      = wall_ms > 0 ? 1000.0 / wall_ms : 0.0;
        const int    N        = tok_.layers.layer_count;
        const double nd       = N > 0 ? static_cast<double>(N) : 1.0;
        auto ms = [](PNanos ns) { return ns / 1e6; };
        auto avg = [&](PNanos ns) { return ns / 1e6 / nd; };

        // Derived groupings
        const double attn_avg_ms = avg(tok_.layers.linear_q_ns + tok_.layers.linear_k_ns +
                                       tok_.layers.linear_v_ns + tok_.layers.rope_ns +
                                       tok_.layers.kv_quant_ns + tok_.layers.attn_scores_ns +
                                       tok_.layers.attn_sub_norm_ns + tok_.layers.linear_o_ns);
        const double mlp_avg_ms  = avg(tok_.layers.linear_gate_ns + tok_.layers.linear_up_ns +
                                       tok_.layers.relu2_ns + tok_.layers.ffn_sub_norm_ns +
                                       tok_.layers.linear_down_ns);
        const double norm_avg_ms = avg(tok_.layers.rmsnorm_pre_ns + tok_.layers.rmsnorm_post_ns);
        const double layer_avg   = attn_avg_ms + mlp_avg_ms + norm_avg_ms;

        // Bottleneck flags
        auto flag = [&](bool cond) -> const char* { return cond ? "  ⚠ " : "    "; };

        err << std::fixed << std::setprecision(2);
        err << "\n[PROF] ──────────────────────────────────────────────────────────────\n";
        err << "[PROF]  Token wall time : " << std::setw(8) << wall_ms << " ms"
            << "   →  tok/s = " << std::setprecision(2) << tps << "\n";
        err << "[PROF]  Workers         : " << pool_.size() << "   |   linear calls : " << tok_.linear_calls << "\n";
        err << "[PROF] ──────────────────────────────────────────────────────────────\n";

        err << "[PROF]  embedding                  : " << std::setw(8) << ms(tok_.embed_ns)   << " ms\n";
        err << "[PROF]  layer avg (×" << std::setw(2) << N << ")            : "
            << std::setw(8) << layer_avg << " ms"
            << "  (attn=" << std::setprecision(2) << attn_avg_ms
            << "  mlp="   << mlp_avg_ms
            << "  norm="  << norm_avg_ms << ")\n";

        err << "[PROF] ┌─ Per-layer averages ──────────────────────────────────────\n";
        err << "[PROF] │  linear Q                 : " << flag(avg(tok_.layers.linear_q_ns)>1.5)   << std::setw(7) << avg(tok_.layers.linear_q_ns)   << " ms/layer\n";
        err << "[PROF] │  linear K                 : " << flag(avg(tok_.layers.linear_k_ns)>1.5)   << std::setw(7) << avg(tok_.layers.linear_k_ns)   << " ms/layer\n";
        err << "[PROF] │  linear V                 : " << flag(avg(tok_.layers.linear_v_ns)>1.5)   << std::setw(7) << avg(tok_.layers.linear_v_ns)   << " ms/layer\n";
        err << "[PROF] │  linear O (attn out)      : " << flag(avg(tok_.layers.linear_o_ns)>1.5)   << std::setw(7) << avg(tok_.layers.linear_o_ns)   << " ms/layer\n";
        err << "[PROF] │  RoPE                     :     " << std::setw(7) << avg(tok_.layers.rope_ns)          << " ms/layer\n";
        err << "[PROF] │  KV cache quant           :     " << std::setw(7) << avg(tok_.layers.kv_quant_ns)      << " ms/layer\n";
        err << "[PROF] │  attn scores+softmax      : " << flag(avg(tok_.layers.attn_scores_ns)>0.5) << std::setw(7) << avg(tok_.layers.attn_scores_ns) << " ms/layer  (seq_len=" << kv_len_ << ")\n";
        err << "[PROF] │  attn sub-norm            :     " << std::setw(7) << avg(tok_.layers.attn_sub_norm_ns) << " ms/layer\n";
        err << "[PROF] │  linear gate              : " << flag(avg(tok_.layers.linear_gate_ns)>1.5) << std::setw(7) << avg(tok_.layers.linear_gate_ns) << " ms/layer\n";
        err << "[PROF] │  linear up               : " << flag(avg(tok_.layers.linear_up_ns)>1.5)   << std::setw(7) << avg(tok_.layers.linear_up_ns)   << " ms/layer\n";
        err << "[PROF] │  relu2 activation        :     " << std::setw(7) << avg(tok_.layers.relu2_ns)          << " ms/layer\n";
        err << "[PROF] │  FFN sub-norm             :     " << std::setw(7) << avg(tok_.layers.ffn_sub_norm_ns)   << " ms/layer\n";
        err << "[PROF] │  linear down              : " << flag(avg(tok_.layers.linear_down_ns)>1.5) << std::setw(7) << avg(tok_.layers.linear_down_ns) << " ms/layer\n";
        err << "[PROF] │  RMSNorm pre-attn         :     " << std::setw(7) << avg(tok_.layers.rmsnorm_pre_ns)   << " ms/layer\n";
        err << "[PROF] │  RMSNorm post-attn        :     " << std::setw(7) << avg(tok_.layers.rmsnorm_post_ns)  << " ms/layer\n";
        err << "[PROF] │  prefetch wait (stalls)   : " << flag(ms(tok_.layers.prefetch_wait_ns)>20) << std::setw(7) << ms(tok_.layers.prefetch_wait_ns) << " ms total\n";
        err << "[PROF] └──────────────────────────────────────────────────────────\n";

        err << "[PROF]  final RMSNorm              : " << std::setw(8) << ms(tok_.final_norm_ns)  << " ms\n";
        err << "[PROF]  argmax logits (vocab=32k)  : " << flag(ms(tok_.argmax_ns)>30)
            << std::setw(7) << ms(tok_.argmax_ns) << " ms\n";

        err << "[PROF] ── linearPacked internals (total across all layers) ──────\n";
        err << "[PROF]  activation quant (A8)      : " << flag(ms(tok_.aq_total_ns)>30)
            << std::setw(7) << ms(tok_.aq_total_ns)   << " ms  (" << tok_.linear_calls << " calls)\n";
        err << "[PROF]  pool.execute (int8 matmul) : " << flag(ms(tok_.pool_total_ns)>100)
            << std::setw(7) << ms(tok_.pool_total_ns)  << " ms\n";

        const double linear_total = ms(tok_.aq_total_ns) + ms(tok_.pool_total_ns);
        const double linear_pct   = wall_ms > 0 ? linear_total / wall_ms * 100.0 : 0.0;
        err << "[PROF]  → linear total            :     " << std::setw(7) << linear_total
            << " ms  (" << std::setprecision(1) << linear_pct << "% of token wall)\n";

        // ---- Bottleneck summary ----
        err << "[PROF] ── Bottleneck Notes ──────────────────────────────────────\n";

        if (ms(tok_.pool_total_ns) / wall_ms > 0.70)
            err << "[PROF]  ⚠  MATMUL BOUND — int8 AVX2 pool.execute dominates. "
                   "Check -mavx2 is active;\n"
                   "[PROF]     consider llama.cpp's optimized Q1.58 kernel.\n";

        if (ms(tok_.argmax_ns) > 25.0)
            err << "[PROF]  ⚠  ARGMAX SLOW — " << std::setprecision(1) << ms(tok_.argmax_ns)
                << " ms decoding vocab=32000. Embed matrix dot not batched;\n"
                   "[PROF]     consider caching lm_head BF16 rows as float32 once.\n";

        if (ms(tok_.layers.prefetch_wait_ns) > 15.0)
            err << "[PROF]  ⚠  I/O STALLS — " << std::setprecision(1) << ms(tok_.layers.prefetch_wait_ns)
                << " ms waiting on prefetch. Weights not fully cached in RAM.\n"
                   "[PROF]     Run extract_bitnet.py and ensure converted/ fits in RAM.\n";

        if (ms(tok_.aq_total_ns) / wall_ms > 0.10)
            err << "[PROF]  ⚠  AQ8 OVERHEAD — activation quantization is "
                << std::setprecision(1) << (ms(tok_.aq_total_ns)/wall_ms*100.0)
                << "% of token time.\n"
                   "[PROF]     This scales with hidden_size. AVX2 path should be fast;\n"
                   "[PROF]     check compiler is emitting vpabsps/vmaxps.\n";

        const double attn_score_total = ms(tok_.layers.attn_scores_ns);
        if (kv_len_ > 200 && attn_score_total > 20.0)
            err << "[PROF]  ⚠  ATTENTION GROWS — " << std::setprecision(1) << attn_score_total
                << " ms for seq_len=" << kv_len_ << ". O(seq·heads·head_dim) per token.\n"
                   "[PROF]     Expected: will double every time seq_len doubles.\n";

        err << "[PROF] ─────────────────────────────────────────────────────────\n\n";
    }
