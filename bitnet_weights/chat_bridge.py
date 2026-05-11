import subprocess
import sys
import os
import re
import json
from tokenizers import Tokenizer

# Path configuration
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
TOKENIZER_PATH = os.path.join(BASE_DIR, "model", "tokenizer.json")
RUN_BITNET_EXE = os.path.join(BASE_DIR, "..", "build", "run_bitnet.exe")
WEIGHTS_DIR = os.path.join(BASE_DIR, "converted")
CONFIG_PATH = os.path.join(BASE_DIR, "model", "config.json")
DEFAULT_MAX_NEW_TOKENS = 50

def load_token_ids(tokenizer):
    bos = tokenizer.token_to_id("<|begin_of_text|>")
    eos = {tokenizer.token_to_id("<|end_of_text|>"), tokenizer.token_to_id("<|eot_id|>")}

    if os.path.exists(CONFIG_PATH):
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            cfg = json.load(f)
        bos = cfg.get("bos_token_id", bos)
        configured_eos = cfg.get("eos_token_id", [])
        if isinstance(configured_eos, int):
            eos.add(configured_eos)
        else:
            eos.update(configured_eos)

    eos.discard(None)
    return bos, eos

def main():
    if not os.path.exists(TOKENIZER_PATH):
        print(f"Error: Tokenizer not found at {TOKENIZER_PATH}")
        return

    print("Loading tokenizer...")
    tokenizer = Tokenizer.from_file(TOKENIZER_PATH)
    bos_token_id, eos_token_ids = load_token_ids(tokenizer)
    
    print("\n=== BitNet b1.58 Chat Mode (Ternary VM) ===")
    print("Type your message and press Enter. Type 'exit' to quit.")
    
    history = [bos_token_id]
    
    while True:
        try:
            user_input = input("\nUser: ")
            if user_input.lower() in ["exit", "quit"]:
                break
            
            # Encode user input
            encoded = tokenizer.encode(user_input, add_special_tokens=False)
            history.extend(encoded.ids)
            
            print("Model is thinking...", end="", flush=True)
            
            # Call C++ inference engine with real-time streaming
            cmd = [
                RUN_BITNET_EXE,
                "--dir", WEIGHTS_DIR,
                "--tokens", str(DEFAULT_MAX_NEW_TOKENS),
                "--temp", "0.7",
                "--penalty", "1.2",
                "--model", os.path.join(BASE_DIR, "model", "model.safetensors"),
            ] + [str(tid) for tid in history]
            
            process = subprocess.Popen(
                cmd, 
                stdout=subprocess.PIPE, 
                stderr=subprocess.PIPE, 
                text=True,
                bufsize=1,
                universal_newlines=True
            )
            
            print("Model: ", end="", flush=True)
            
            # Capture and decode tokens in real-time
            full_response_ids = []
            while True:
                line = process.stdout.readline()
                if not line and process.poll() is not None:
                    break
                if line:
                    # Parse predicted token ID
                    match = re.search(r"Next predicted token: (\d+)", line)
                    if match:
                        next_token = int(match.group(1))
                        full_response_ids.append(next_token)
                        history.append(next_token)
                        
                        decoded = tokenizer.decode([next_token], skip_special_tokens=True)
                        print(f"{decoded}", end="", flush=True)
                        if next_token in eos_token_ids:
                            break
                    
                    # Capture and print stats at the end
                    if "=== Generation Stats ===" in line:
                        print("\n" + "-"*40)
                        while True:
                            stat_line = process.stdout.readline()
                            if not stat_line or "====" in stat_line:
                                break
                            print(stat_line.strip())
                        print("-"*40)
                        break
            
            stderr = process.stderr.read()
            if process.returncode != 0 and not full_response_ids:
                print(f"\nError in inference: {stderr}")
                
            print() # New line after response
            
        except KeyboardInterrupt:
            break
        except Exception as e:
            print(f"\nError: {e}")

if __name__ == "__main__":
    main()
