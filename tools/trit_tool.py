#!/usr/bin/env python3
"""The single current-platform CLI. CMake executes gates; diagnostics exports proof."""
from __future__ import annotations
import argparse
import datetime
import hashlib
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD_DIR = ROOT / 'build_current_cleanup'
CONTRACT = dict(isa_version=2, executable_abi_version=3, function_abi_version=3,
                syscall_abi_version=2, vector_abi_version=1, tboot_version=3, tdisk_version=2)
MANIFEST_SCHEMAS = {'ARCHITECTURE_MANIFEST.json':'trit.architecture_manifest.v1',
 'IMAGE_FORMAT_MANIFEST.json':'trit.image_format_manifest.v2', 'APP_MANIFEST.json':'trit.apps.v1',
 'SYSCALL_MANIFEST.json':'trit.syscalls.v2'}
FNV = 'FNV-1a-64'
# Exact accepted current source closure, independently checked against local includes.
SOURCE_INPUTS = ['APP_MANIFEST.json', 'ARCHITECTURE_MANIFEST.json', 'CMakeLists.txt', 'IMAGE_FORMAT_MANIFEST.json', 'SYSCALL_MANIFEST.json', 'TEST_MANIFEST.json', 'apps/about.trit', 'apps/bin_core.trit', 'apps/calculator.trit', 'apps/clear.trit', 'apps/crash.trit', 'apps/date.trit', 'apps/desktop.trit', 'apps/doctor.trit', 'apps/file_manager.trit', 'apps/fsck.trit', 'apps/help.trit', 'apps/init.trit', 'apps/kill.trit', 'apps/libwidget.trit', 'apps/log.trit', 'apps/login.trit', 'apps/ls.trit', 'apps/mount.trit', 'apps/os_sdk.trit', 'apps/paint.trit', 'apps/passwd.trit', 'apps/ps.trit', 'apps/reboot.trit', 'apps/service_host.trit', 'apps/service_runtime.trit', 'apps/service_stub.trit', 'apps/settings.trit', 'apps/shell.trit', 'apps/shutdown.trit', 'apps/sleep.trit', 'apps/sync.trit', 'apps/sysinfo.trit', 'apps/task_manager.trit', 'apps/terminal.trit', 'apps/test.trit', 'apps/text_editor.trit', 'architecture_contract.h', 'architecture_contract.trit', 'architecture_instruction_codec.h', 'architecture_isa_v2.h', 'architecture_v2_assembler_directives.h', 'architecture_v2_assembler_types.h', 'architecture_v2_support.h', 'build_tos_image.cpp', 'executable_header_common.h', 'executable_header_v3.h', 'kernel.trit', 'native_kernel_trap_stub.tasm', 'ternary_asm.h', 'ternary_backend.h', 'ternary_binary_io.h', 'ternary_compiler.h', 'ternary_compiler_ast.h', 'ternary_compiler_cfg.h', 'ternary_compiler_codegen.h', 'ternary_compiler_ir.h', 'ternary_compiler_lexer.h', 'ternary_compiler_parser.h', 'ternary_compiler_types.h', 'ternary_dataflow.trit', 'ternary_host_runtime.h', 'ternary_ir.h', 'ternary_isa.h', 'ternary_lanes.h', 'ternary_math.h', 'ternary_native_ops.h', 'ternary_os.h', 'ternary_redo_wal.h', 'ternary_scalar.h', 'ternary_simd.h', 'ternary_stream.trit', 'ternary_symbolic_encoding.h', 'ternary_uint128.h', 'ternary_vm.h', 'ternary_vm_state.h', 'tests/current_only_conformance.cpp', 'tools/trit_tool.py', 'tritc.cpp']
TEST_TEMPLATE = {'schema_version': 1, 'platform': {'isa_version': 2, 'executable_abi_version': 3, 'function_abi_version': 3, 'syscall_abi_version': 2, 'vector_abi_version': 1, 'tboot_version': 3, 'tdisk_version': 2}, 'conformance': {'source': 'tests/current_only_conformance.cpp', 'target': 'trit_current_conformance', 'ctest_name': 'trit_current_conformance', 'selectors': ['--cli-only', '--clock-idle-input-only', '--clock-kernel-only', '--clock-only', '--desktop-only', '--extent-lifecycle-only', '--metrics-only', '--numeric-only', '--sdk-only', '--service-auto-modes-only', '--service-auto-only', '--service-coexistence-only', '--service-edges-only', '--service-errors-only', '--service-fixtures-only', '--service-history-cap1-only', '--service-link-only', '--service-only', '--service-reservation-only', '--stream-only', '--wal-admission-only', '--wal-only', '--wal-shape-only'], 'default_workflows': ['wal_full', 'sdk', 'numeric', 'metrics', 'clock_kernel', 'clock_boundaries', 'clock_idle_input', 'clock_service', 'desktop', 'stream', 'service_full', 'cli'], 'separate_required_workflows': ['service_auto'], 'timeout_seconds': 0}, 'build': {'directory': 'build_current_cleanup', 'configure': ['cmake', '-S', '.', '-B', 'build_current_cleanup'], 'targets': {'current_images': {'direct_dependencies': ['release/TernaryOS/ternary-os.tboot', 'release/TernaryOS/ternary-os.tdisk'], 'producer': 'build_tos_image'}, 'current_validate': {'direct_dependencies': ['trit_current_conformance'], 'runs': 'trit_current_conformance'}, 'ci_production': {'direct_dependencies_if_BUILD_TESTING': ['tritc', 'build_tos_image', 'ctest'], 'direct_dependencies_if_not_BUILD_TESTING': ['tritc', 'build_tos_image', 'current_validate']}, 'stage_tos_release': {'available_if': 'TRIT_ENABLE_SDL_RELEASE AND SDL2_FOUND AND run_tos_sdl.cpp exists', 'direct_dependencies': ['current_images', 'run_tos_sdl']}, 'smoke_tos_release': {'available_if': 'stage_tos_release available', 'direct_dependencies': ['stage_tos_release'], 'frames': 10}}}, 'cli': {'source': 'tools/trit_tool.py', 'commands': ['doctor', 'contract-check', 'test', 'export-diagnostics']}}
SERVICE_REPORT = 'service_workflow_artifact/service/report.json'
GROUPS = {
 'wal_full':('service_workflow_artifact/wal/report.json','full'),
 'wal_admission':('service_workflow_artifact/wal/admission_low/report.json','focused'),
 'wal_shape':('service_workflow_artifact/wal/checkpoint_shapes/checkpoint_shapes_report.json','focused'),
 'wal_extent':('service_workflow_artifact/wal/extent_lifecycle/extent_lifecycle_report.json','focused'),
 'service_full':(SERVICE_REPORT,'full'), 'service_errors':(SERVICE_REPORT,'focused'),
 'service_fixtures':(SERVICE_REPORT,'focused'), 'service_history_cap1':(SERVICE_REPORT,'focused'),
 'service_reservation':(SERVICE_REPORT,'focused'), 'service_edges':(SERVICE_REPORT,'focused'),
 'service_link':('service_workflow_artifact/service_link_preflight/link_preflight_report.json','compile-link-only'),
 'service_auto':('service_workflow_artifact/service/automatic_start/automatic_report.json','full'),
 'service_auto_focused':('service_workflow_artifact/review/automatic_start/focused/automatic_report.json','focused-automatic-three'),
 'service_coexistence':('service_workflow_artifact/service/coexistence/coexistence_report.json','focused-coexistence'),
 'sdk':('service_workflow_artifact/sdk/sdk_report.json','full'),
 'metrics':('service_workflow_artifact/metrics/phase2_report.json','full'),
 'stream':('service_workflow_artifact/stream/phase1_report.json','full'),
 'numeric':('numeric_workflow_artifact/report.json','full'), 'desktop':(None,'full'),
 'clock_kernel':('service_workflow_artifact/clock/kernel/report.json','full'),
 'clock_boundaries':('service_workflow_artifact/clock/kernel_boundaries/report.json','full'),
 'clock_idle_input':('service_workflow_artifact/clock/idle_input/report.json','full'),
 'clock_service':('service_workflow_artifact/clock/service/report.json','focused'),
 'cli':('service_workflow_artifact/final_gate/cli_e2e/scenario_report.json','full')}
MODES=['normal','spawn_failure','startup_failure']
WAL=['live_ownership','untracked_data','unknown','outside_used','boot_identity','boot_value','boot_txid','boot_version','corrupt','broken_chain','invalid_highwater','invalid_domain','low_corrupt','low_broken_chain','low_invalid_highwater','low_invalid_domain']
SHAPES=['shape_sequential_closed','shape_sequential_closed_recovery','shape_interleaved','shape_interleaved_recovery','shape_reverse_closure','shape_reverse_closure_recovery','shape_duplicate_commit','shape_late_data','shape_aborted_zero','shape_aborted_zero_recovery']
EXTENT=['extent_lifecycle','extent_lifecycle_recovery']
WAL_WORK=['pressure','no_device','targeted_fsync','blocked_prefix','resumed_prefix_recovery','replay_orphan','replay_torn_chain','recovered_orphan','recovered_torn_chain','admission_io_failure','admission_fitting_io_failure']+['recovery_'+str(i) for i in range(4)]+['publication_failure_'+str(i) for i in range(1,5)]+['failed_publication_recovery_'+str(i) for i in range(1,7)]
SERVICE_FILES=['reconciliation.json','state.jsonl','progress.jsonl','cleanup.jsonl','embedded_guest.trit','guest.linked.tasm','guest.tboot','disk.tdisk','vm_state.bin','svc.0.json','svc.1.json','svc.gen.json','svc.report.json']
ZERO_LOGS=['accepted_records.jsonl','committed_records.jsonl','jobs.jsonl','retirement.jsonl','syscalls.jsonl']
CONSTRUCTOR_REJECT=['zero_channel','pool1','pool2','pool3','pool4']
GUI_FILES=['manifest.json','process_table.json','window_table.json','input_journal.jsonl','framebuffer_snapshot.png','checkpoint/checkpoint.json','checkpoint/input_journal.jsonl','checkpoint/vm_state.bin']
HOST_FILES=['host_before/checkpoint.json','host_before/vm_state.bin','host_before/syscall_trace.jsonl','host_after/checkpoint.json','host_after/vm_state.bin','host_after/syscall_trace.jsonl','health/process_table.json']
AUTO_FILES=['automatic_boot/attempts.jsonl','host_before/automatic_observation.json','host_after/automatic_observation.json']
FIELDS={'schema_version','group','run_id','selector','scope','state','outcome','started_utc','finished_utc','contract','source_inputs','evidence_files','required_scenarios'}

def read(relative):
    try: return (ROOT/relative).read_text(encoding='utf-8')
    except (OSError,UnicodeError) as error: raise ValueError(f'{relative}: unreadable current input: {error}') from None

def load(relative):
    try: data=json.loads(read(relative))
    except json.JSONDecodeError as error: raise ValueError(f'{relative}: malformed JSON at line {error.lineno}: {error.msg}') from None
    if not isinstance(data,dict): raise ValueError(f'{relative}: expected JSON object')
    return data

def shape(data, path, expected):
    value=data
    for field in path.split('.'):
        if not isinstance(value,dict) or field not in value: raise ValueError(f'{path}: required field missing')
        value=value[field]
    if type(value) is not expected: raise ValueError(f'{path}: expected {expected.__name__}')
    return value

def selectors():
    return sorted(set(re.findall(r'argv\[i\]\)\s*==\s*"(--[a-z0-9-]+)"',read('tests/current_only_conformance.cpp'))))

def manifests():
    values={};errors=[]
    for name in [*MANIFEST_SCHEMAS,'TEST_MANIFEST.json']:
        try:
            obj=load(name)
            if name=='TEST_MANIFEST.json':
                if type(obj.get('schema_version')) is not int or obj['schema_version']!=1: raise ValueError('schema_version must be 1')
                for path,typ in [('platform',dict),*[("platform."+field,int) for field in CONTRACT],('conformance',dict),('build',dict),('cli',dict),('conformance.selectors',list),('conformance.timeout_seconds',int),('cli.commands',list)]: shape(obj,path,typ)
            else:
                if obj.get('schema')!=MANIFEST_SCHEMAS[name]: raise ValueError('schema must be '+MANIFEST_SCHEMAS[name])
                required={'ARCHITECTURE_MANIFEST.json':[('architecture',dict),('architecture.isa_version',int),('abi',dict),('abi.executable_version',int),('abi.function_abi_version',int),('abi.syscall_abi_version',int),('abi.vector_abi_version',int)],'IMAGE_FORMAT_MANIFEST.json':[('formats',dict),('formats.tboot',dict),('formats.tdisk',dict)],'APP_MANIFEST.json':[('apps',list)],'SYSCALL_MANIFEST.json':[('runtime_mappings',dict),('aliases',dict),('sdk_wrappers',dict),('syscall_abi_version',int)]}[name]
                for path,typ in required: shape(obj,path,typ)
            values[name]=obj
        except ValueError as error: errors.append(f'{name}: {error}')
    return values,errors

def local_closure():
    # Repository-local quoted includes only; OS/system headers are external.
    app=load('APP_MANIFEST.json')
    result={'APP_MANIFEST.json','ARCHITECTURE_MANIFEST.json','CMakeLists.txt','IMAGE_FORMAT_MANIFEST.json','SYSCALL_MANIFEST.json','TEST_MANIFEST.json','architecture_contract.h','architecture_contract.trit','kernel.trit','native_kernel_trap_stub.tasm','ternary_dataflow.trit','ternary_stream.trit','apps/os_sdk.trit','apps/libwidget.trit','apps/service_runtime.trit','tools/trit_tool.py'}
    result.update('apps/'+row['source'] for row in app['apps'])
    pending=['tests/current_only_conformance.cpp','tritc.cpp','build_tos_image.cpp']
    while pending:
        name=pending.pop()
        if name in result: continue
        result.add(name)
        for include in re.findall(r'^\s*#\s*include\s*"([^"\n]+)"',read(name),re.M):
            options=[pathlib.PurePosixPath(name).parent/include,pathlib.PurePosixPath(include)]
            found=next((str(p) for p in options if (ROOT/p).is_file()),None)
            if not found: raise ValueError(f'{name}: unresolved quoted include {include}')
            if found not in result: pending.append(found)
    return sorted(result)

def inputs_errors(values):
    errors=[]
    for name in SOURCE_INPUTS:
        try:
            if not read(name): errors.append(f'{name}: current input is empty')
        except ValueError as error: errors.append(str(error))
    app=values.get('APP_MANIFEST.json',{})
    for index,row in enumerate(app.get('apps',[])):
        if not isinstance(row,dict): errors.append(f'APP_MANIFEST.json apps[{index}]: expected object');continue
        for field in ['source','id','title','guest_path']:
            if not isinstance(row.get(field),str) or not row[field]:errors.append(f'APP_MANIFEST.json apps[{index}].{field}: expected nonempty string')
        if isinstance(row.get('source'),str):
            path=pathlib.PurePosixPath(row['source'])
            if path.is_absolute() or '..' in path.parts or '\\' in row['source'] or re.match(r'^[a-zA-Z]:',row['source']) or not (ROOT/'apps'/path).is_file():errors.append(f'APP_MANIFEST.json apps[{index}].source: missing or unsafe apps/{path}')
    return errors

def fail(errors):
    if errors: print('current platform errors:\n'+'\n'.join('- '+e for e in errors));return 1
    return 0

def doctor():
    values,errors=manifests();errors+=inputs_errors(values)
    if not fail(errors):print('current platform files and manifest shapes: OK');return 0
    return 1

def check_contracts():
    values,errors=manifests();errors+=inputs_errors(values)
    if errors:return fail(errors)
    def check(ok,message):
        if not ok:errors.append(message)
    try:
        a=values['ARCHITECTURE_MANIFEST.json'];f=values['IMAGE_FORMAT_MANIFEST.json'];app=values['APP_MANIFEST.json'];syscalls=values['SYSCALL_MANIFEST.json'];test=values['TEST_MANIFEST.json']
        check(a['architecture']['isa_version']==2,'ARCHITECTURE_MANIFEST.json architecture.isa_version must be 2')
        for name,value in [('executable_version',3),('function_abi_version',3),('syscall_abi_version',2),('vector_abi_version',1)]:check(a['abi'].get(name)==value,'ARCHITECTURE_MANIFEST.json abi.'+name+' must be '+str(value))
        for name,version in [('tboot',3),('tdisk',2)]:
            obj=f['formats'][name];check(obj.get('write_version')==version and obj.get('read_versions')==[version],'IMAGE_FORMAT_MANIFEST.json '+name+' must read/write current version only')
            check(not any('migration' in key for key in obj),'IMAGE_FORMAT_MANIFEST.json '+name+' contains retired migration field')
        builder=read('build_tos_image.cpp')
        actual=[(x[1],x[0]+'.trit',x[2],x[3]) for x in re.findall(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"(/bin/[^"]+)"',builder)]
        declared=[(row['id'],row['source'],row['title'],row['guest_path']) for row in app['apps']]
        check(len({row[0] for row in declared})==len(declared),'APP_MANIFEST.json duplicate app identity')
        check(len({row[3] for row in declared})==len(declared),'APP_MANIFEST.json duplicate guest path')
        check(sorted(actual)==sorted(declared),'APP_MANIFEST.json app source/id/title/path mapping differs from build_tos_image.cpp registrations')
        runtime={}
        for name,value in re.findall(r'static constexpr int\s+(\w+)\s*=\s*(\w+)\s*;',read('ternary_compiler_ir.h')):
            if name.startswith(('sys_','reserved_')):runtime[name]=int(value) if value.isdigit() else runtime.get(value)
        check(syscalls['runtime_mappings']==runtime,'SYSCALL_MANIFEST.json runtime_mappings differ from current runtime constants/aliases/reserved IDs')
        check(syscalls['syscall_abi_version']==2,'SYSCALL_MANIFEST.json syscall_abi_version must be 2')
        aliases={name:value for name,value in re.findall(r'static constexpr int\s+(sys_\w+)\s*=\s*(sys_\w+)\s*;',read('ternary_compiler_ir.h'))}
        check(syscalls['aliases']==aliases,'SYSCALL_MANIFEST.json aliases differ from runtime')
        compiler=read('ternary_compiler_codegen.h');sdk=read('apps/os_sdk.trit')
        service=re.search(r'static\s+int\s+runtimeService\s*\(\s*const\s+std::string&\s+name\s*\)\s*\{',compiler)
        body=''
        if service:
            begin=service.end();depth=1;end=begin
            while depth and end<len(compiler):depth+=(compiler[end]=='{')-(compiler[end]=='}');end+=1
            body=compiler[begin:end-1] if depth==0 else ''
        pairs=re.findall(r'\bif\s*\(\s*name\s*==\s*"(sys_\w+)"\s*\)\s*return\s+runtime::(sys_\w+)\s*;',body)
        actual_compiler=dict(pairs);expected_compiler={name:name for name in runtime if name.startswith('sys_')}
        check(len(pairs)==len(actual_compiler) and set(actual_compiler)==set(expected_compiler),'SYSCALL_MANIFEST.json compiler mapping names differ from runtimeService')
        for name,target in expected_compiler.items():
            check(actual_compiler.get(name)==target,'SYSCALL_MANIFEST.json compiler mapping differs for '+name+': expected runtime::'+target+', got '+str(actual_compiler.get(name)))
        for wrapper,calls in syscalls['sdk_wrappers'].items():
            start=re.search(r'\bfn\s+'+re.escape(wrapper)+r'\s*\(',sdk)
            if not start:check(False,'SYSCALL_MANIFEST.json SDK wrapper missing '+wrapper);continue
            begin=sdk.find('{',start.start());depth=1;end=begin+1
            while depth and end<len(sdk):depth+=(sdk[end]=='{')-(sdk[end]=='}');end+=1
            check(isinstance(calls,list) and all(isinstance(call,str) and re.search(r'\b'+re.escape(call)+r'\s*\(',sdk[begin:end]) for call in calls),'SYSCALL_MANIFEST.json SDK mapping differs for '+wrapper)
        direct={}
        for found in re.finditer(r'\bfn\s+(\w+)\s*\(',sdk):
            begin=sdk.find('{',found.start());depth=1;end=begin+1
            while depth and end<len(sdk):depth+=(sdk[end]=='{')-(sdk[end]=='}');end+=1
            calls=sorted(set(re.findall(r'\b(sys_\w+)\s*\(',sdk[begin:end])))
            if calls:direct[found.group(1)]=calls
        declared=syscalls['sdk_wrappers']
        check(set(declared)==set(direct) and all(isinstance(calls,list) and sorted(set(calls))==direct.get(name) and len(calls)==len(set(calls)) for name,calls in declared.items()),'SYSCALL_MANIFEST.json sdk_wrappers must describe every current direct syscall SDK wrapper')
        expected=json.loads(json.dumps(TEST_TEMPLATE));expected['conformance']['selectors']=selectors()
        check(test==expected,'TEST_MANIFEST.json schema/graph/selector/CLI mapping differs from current CMake/source contract')
        cmake=read('CMakeLists.txt')
        for token in ['add_executable(tritc tritc.cpp)','add_executable(build_tos_image build_tos_image.cpp)','tests/current_only_conformance.cpp','add_test(NAME trit_current_conformance','PROPERTIES TIMEOUT 0','add_custom_target(current_validate','add_custom_target(ci_production DEPENDS tritc build_tos_image ctest)','add_custom_target(ci_production DEPENDS tritc build_tos_image current_validate)','DEPENDS current_images run_tos_sdl','DEPENDS stage_tos_release']:
            check(token in cmake,'TEST_MANIFEST.json CMake graph mismatch: '+token)
        check(local_closure()==SOURCE_INPUTS,'current source-input closure differs from accepted84 paths; refresh completion provenance explicitly')
        snapshots={'architecture_contract.h':{'ISA_VERSION':'isa_version','CURRENT_EXECUTABLE_VERSION':'executable_abi_version','CURRENT_FUNCTION_ABI_VERSION':'function_abi_version','SYSCALL_ABI_VERSION':'syscall_abi_version','TBOOT_WRITE_VERSION':'tboot_version','TDISK_WRITE_VERSION':'tdisk_version'},'architecture_contract.trit':{'ARCH_ISA_VERSION':'isa_version','ARCH_EXECUTABLE_VERSION':'executable_abi_version','ARCH_FUNCTION_ABI_VERSION':'function_abi_version','ARCH_SYSCALL_ABI_VERSION':'syscall_abi_version'}}
        for path,constants in snapshots.items():
            snapshot=read(path)
            for name,field in constants.items():
                declaration=(r'\binline\s+constexpr\s+int\s+'+re.escape(name)+r'\s*=\s*(-?\d+)\s*;') if path.endswith('.h') else (r'\bconst\s+'+re.escape(name)+r'\s*:\s*t40\s*=\s*(-?\d+)\s*;')
                values=re.findall(declaration,snapshot)
                check(len(values)==1 and int(values[0])==CONTRACT[field],path+' current version '+name+' must equal '+str(CONTRACT[field]))
    except (KeyError,TypeError,ValueError) as error:errors.append('manifest contract field: '+str(error))
    if not fail(errors):print('current platform contracts, registries and graph: OK');return 0
    return 1

def fnv(data):
    value=14695981039346656037
    for byte in data:value=((value^byte)*1099511628211)&((1<<64)-1)
    return f'{value:016x}'

def service_names():
    text=read('tests/current_only_conformance.cpp')
    block=text.split('std::vector<ServiceScenario> serviceScenarios()',1)[1].split('bool runServiceWorkflow(',1)[0]
    names=['history_cap1','history_cap2']+re.findall(r'cases\.push_back\(\{\s*"([a-z0-9_]+)"\s*,',block)
    for pairs in re.findall(r'std::vector<std::pair<std::string,int>>\{(.*?)\}\}',block,re.S):
        names+=re.findall(r'"([a-z0-9_]+)"\s*,\s*\d+',pairs)
    for values in re.findall(r'for\(const auto& name\s*:\s*\{([^}]+)\}',block):
        names+=re.findall(r'"([a-z0-9_]+)"',values)
    marker=re.search(r'for\(int marker_case:\{([^}]+)\}',block)
    if marker:names+=['generation_marker_'+n.strip() for n in marker.group(1).split(',')]
    return names

def expected_names(group):
    service=service_names()
    return {'service_full':service,'service_auto':service+MODES,'service_errors':['permanent_write','fsync_failure'],'service_fixtures':['tokens_reuse_restart','control_full_stall_cancel'],'service_history_cap1':['history_cap1'],'service_reservation':['reserved_head_full_pool'],'service_edges':[s for s in service if not s.startswith('history_cap')],'service_link':service,'service_auto_focused':MODES,'wal_full':WAL,'wal_admission':WAL[-4:],'wal_shape':SHAPES,'wal_extent':EXTENT,'clock_service':['clock_cap1','clock_cap2'],'service_coexistence':['coexistence']}.get(group,[])

def required_files(group,names,scope):
    required={};report=GROUPS[group][0]
    if report:required[report]=False
    def add(root,leaves,empty=False):
        for leaf in leaves:required[root+'/'+leaf]=empty
    def gui(root,host=False,auto=False):
        add(root,GUI_FILES)
        if host:add(root,HOST_FILES)
        if auto:add(root,AUTO_FILES)
    if group.startswith('service_') and group not in ['service_auto_focused','service_link','service_coexistence'] or group=='clock_service':
        root='service_workflow_artifact/clock/service' if group=='clock_service' else 'service_workflow_artifact/service'
        for name in names:
            if name in MODES:continue
            add(root+'/'+name,SERVICE_FILES);add(root+'/'+name,ZERO_LOGS,True)
            if name in CONSTRUCTOR_REJECT:required[root+'/'+name+'/cleanup.jsonl']=True
            if group=='clock_service':add(root+'/'+name,['clock_report.json'])
        if group=='service_full':gui(root+'/coexistence/desktop',True)
    if group in ['service_auto','service_auto_focused']:
        root=str(pathlib.PurePosixPath(report).parent)
        for mode in MODES:add(root+'/'+mode,['mode_report.json']);gui(root+'/'+mode+'/desktop',True,True)
    if group=='service_coexistence':gui('service_workflow_artifact/service/coexistence/desktop',True)
    if group=='desktop':gui('desktop_workflow_artifact');add('service_workflow_artifact/desktop_input',['initial_images.json','initial.tboot','initial.tdisk'])
    if group.startswith('wal_'):
        root=str(pathlib.PurePosixPath(report).parent)
        cases=names if group!='wal_full' else WAL+SHAPES+EXTENT+WAL_WORK
        for case in cases:
            add(root+'/'+case,['boot_result.json','boot.tboot','boot.tasm','live.tdisk'])
            if case=='targeted_fsync':add(root+'/'+case,['frames.json'])
            elif case.startswith('replay_'):add(root+'/'+case,['durable_crash.tdisk'])
            else:add(root+'/'+case,['result.json'])
        if group=='wal_full':
            add(root,['checkpoint_shapes_report.json','extent_lifecycle_report.json','pressure/storage.jsonl','pressure/syscalls.jsonl','pressure/before_checkpoint_stage1.tdisk','pressure/before_checkpoint_stage2.tdisk','pressure/before_checkpoint_stage3.tdisk','pressure/before_checkpoint_stage4.tdisk','blocked_prefix/snapshot_release.jsonl','extent_lifecycle/lifecycle.jsonl','extent_lifecycle/cursor_injections.jsonl'])
    extras={'sdk':['embedded_guest.trit','guest.linked.tasm','sdk.tboot','raw_return_records.jsonl','sdk_results.json','vfs_content.json','disk.tdisk'],'metrics':['embedded_guest.trit','guest.linked.tasm','metrics.tboot','timer_transitions.jsonl','kernel_assertions.json','idle_accounting.json','guest_report.json','busy_syscall_accounting.json','vm_run_accounting.json'],'stream':['embedded_guest.trit','guest.linked.tasm','source_hashes.json','stream-events.json'], 'clock_kernel':['embedded_guest.trit','guest.linked.tasm','clock.tboot','execution.json','actual_boot_rate.jsonl','actual_waits.jsonl','queue_transitions.jsonl','terminal_state.bin'], 'clock_boundaries':['embedded_guest.trit','guest.linked.tasm','clock.tboot','execution.json','actual_boot_rate.jsonl','actual_waits.jsonl','queue_transitions.jsonl','terminal_state.bin'], 'clock_idle_input':['boot.tboot','phases.jsonl','eret_observations.jsonl','prefix_observations.jsonl','prefix_state.bin','guest0.trit','guest1.trit','guest2.trit'],'numeric':['guest.trit','guest_O0.tasm','guest_O1.tasm','guest_O2.tasm','accumulator.tasm','mid_reduction.bin']}
    if group in extras:add(str(pathlib.PurePosixPath(report).parent),extras[group])
    return required

SELECTORS={
 'wal_full':['default','--wal-only'],'wal_admission':['--wal-admission-only'],'wal_shape':['--wal-shape-only'],'wal_extent':['--extent-lifecycle-only'],
 'service_full':['default','--service-only'],'service_auto':['--service-auto-only'],'service_errors':['--service-errors-only'],
 'service_fixtures':['--service-fixtures-only'],'service_history_cap1':['--service-history-cap1-only'],'service_reservation':['--service-reservation-only'],
 'service_edges':['--service-edges-only'],'service_link':['--service-link-only'],'service_auto_focused':['--service-auto-modes-only'],'service_coexistence':['--service-coexistence-only'],
 'sdk':['default','--sdk-only'],'numeric':['default','--numeric-only'],'metrics':['default','--metrics-only'],'stream':['default','--stream-only'],
 'desktop':['default','--desktop-only'],'clock_kernel':['default','--clock-only','--clock-kernel-only'],'clock_boundaries':['default','--clock-only'],
 'clock_idle_input':['default','--clock-idle-input-only'],'clock_service':['default','--clock-only'],'cli':['default','--cli-only']}

def utc_time(value):
    if not isinstance(value,str):raise ValueError('UTC timestamp must be a string')
    parsed=datetime.datetime.fromisoformat(value.replace('Z','+00:00'))
    if parsed.tzinfo is None or parsed.utcoffset()!=datetime.timedelta(0):raise ValueError('timestamp must use UTC offset')
    return parsed

def verify_group(group,source_hashes):
    result={'status':'missing','current_source_match':False,'scope':None,'reasons':[],'completion':f'service_workflow_artifact/completion/{group}.json'}
    path=BUILD_DIR/result['completion']
    if not path.is_file():result['reasons']=['coupled completion missing; report/hash presence is not run proof'];return result
    def status(name,reason):result['status']=name;result['reasons'].append(reason);return result
    try:
        side=json.loads(path.read_text(encoding='utf-8'))
        if not isinstance(side,dict) or set(side)!=FIELDS:return status('invalid','completion requires exact settled fields')
        if type(side['schema_version']) is not int or side['schema_version']!=1 or side['group']!=group or not all(isinstance(side[x],str) and side[x] for x in ['run_id','selector','scope','started_utc']):return status('invalid','invalid completion identity/types')
        if not isinstance(side['source_inputs'],dict) or not isinstance(side['evidence_files'],dict) or not isinstance(side['required_scenarios'],list):return status('invalid','invalid completion maps/coverage')
        for item in side['source_inputs'].values():
            if not isinstance(item,dict) or set(item)!={'algorithm','hash','present'} or item.get('algorithm')!=FNV or type(item.get('present')) is not bool or not re.fullmatch('[0-9a-f]{16}',str(item.get('hash',''))):return status('invalid','invalid mandatory source input record')
        if not isinstance(side['contract'],dict) or set(side['contract'])!=set(CONTRACT) or any(type(v) is not int for v in side['contract'].values()):return status('invalid','invalid platform contract field types')
        result['scope']=side['scope']
        started=utc_time(side['started_utc'])
        if not isinstance(side['contract'],dict):return status('invalid','contract must be an object')
        if side['state']=='pending':
            if side['outcome'] is not None or side['finished_utc'] is not None or side['evidence_files']:return status('invalid','pending completion has complete fields')
            return status('pending','workflow began but closed completion is absent')
        if side['state']!='complete' or side['outcome'] not in ['PASS','FAIL'] or not isinstance(side['finished_utc'],str) or not side['finished_utc']:return status('invalid','invalid completed outcome/time')
        if utc_time(side['finished_utc'])<started:return status('invalid','completion finished before start')
        if side['contract']!=CONTRACT or any(type(v) is not int for v in side['contract'].values()):return status('stale','completion platform tuple mismatch')
        if side['selector'] not in SELECTORS[group]:return status('stale','completion selector does not invoke this group')
        matches=set(side['source_inputs'])==set(SOURCE_INPUTS)
        for name,actual in source_hashes.items():
            item=side['source_inputs'].get(name)
            matches=matches and isinstance(item,dict) and item.get('algorithm')==FNV and item.get('present') is True and item.get('hash')==actual and actual is not None
        result['current_source_match']=bool(matches)
        if not matches:return status('stale','current source closure/digest mismatch')
        expected=expected_names(group);names=side['required_scenarios']
        if sorted(names)!=sorted(expected) or len(names)!=len(set(names)):return status('stale','required scenario coverage mismatch')
        allowed=[GROUPS[group][1]]
        if group in ['desktop','sdk','numeric','metrics','stream','clock_kernel','clock_boundaries','clock_idle_input','cli']:allowed+=['focused']
        if side['scope'] not in allowed:return status('stale','full/focused scope mismatch')
        if side['outcome']=='FAIL':return status('failed','actual outer workflow returned false')
        required=required_files(group,names,side['scope'])
        if not set(required)<=set(side['evidence_files']):return status('stale','required closed evidence file missing from completion')
        for name,item in side['evidence_files'].items():
            relative=pathlib.PurePosixPath(name)
            if not isinstance(name,str) or relative.is_absolute() or '..' in relative.parts or '\\' in name or re.match(r'^[a-zA-Z]:',name) or relative.as_posix()!=name:return status('invalid','unsafe evidence path')
            if not isinstance(item,dict) or item.get('algorithm')!=FNV or type(item.get('size_bytes')) is not int or not re.fullmatch('[0-9a-f]{16}',str(item.get('hash',''))):return status('invalid','invalid evidence digest/size')
            file=BUILD_DIR/relative
            if not file.is_file():return status('stale','closed evidence removed: '+name)
            data=file.read_bytes()
            if len(data)!=item['size_bytes'] or fnv(data)!=item['hash']:return status('stale','closed evidence bytes changed: '+name)
            if not data and not required.get(name,False):return status('stale','required evidence is empty: '+name)
        report_path=GROUPS[group][0]
        if report_path:
            report=json.loads((BUILD_DIR/report_path).read_text())
            passed=str(report.get('status','')).upper()=='PASS' or report.get('pass') is True
            if not passed:return status('stale','report outcome does not match completed PASS')
            if group in ['service_full','service_errors','service_fixtures','service_history_cap1','service_reservation','service_edges','clock_service']:
                if report.get('scope')!=side['scope'] or report.get('scenario_count')!=len(expected) or sorted(report.get('required_scenarios',[]))!=sorted(expected) or sorted(report.get('selected_scenarios',[]))!=sorted(expected):return status('stale','service report coverage/scope mismatch')
                if group=='service_full' and report.get('coexistence_requested') is not True:return status('stale','full service coexistence missing')
            if group=='wal_full' and (report.get('scope')!='full' or report.get('guard_scenario_count')!=16 or sorted(report.get('selected_guard_cases',[]))!=sorted(WAL)):return status('stale','full WAL16 report coverage mismatch')
            if group in ['service_auto','service_auto_focused']:
                modes=report.get('automatic_modes',[])
                if len(modes)!=3 or sorted(row.get('mode') for row in modes)!=sorted(MODES) or any(row.get('status')!='PASS' for row in modes):return status('stale','automatic three-mode coverage mismatch')
                for mode in MODES:
                    mode_report=json.loads((BUILD_DIR/pathlib.PurePosixPath(report_path).parent/mode/'mode_report.json').read_text())
                    if mode_report.get('status')!='PASS' or mode_report.get('exact_desktop_passed') is not True or mode_report.get('image_generated') is not True:return status('stale','automatic mode outcome mismatch')
            if group=='service_link':
                if report.get('runtime_executed') is not False or report.get('scope')!='compile-link-only' or any(report.get(key)!=33 for key in ['requested','attempted_count','compiled_count','linked_count']) or any(sorted(report.get(key,[]))!=sorted(expected) for key in ['attempted','compiled','linked']):return status('stale','link proof requires exact33 compiled/linked and no runtime claim')
            if group=='service_auto':
                service=service_names()
                if report.get('scope')!='full' or report.get('scenario_count')!=33 or sorted(report.get('required_scenarios',[]))!=sorted(service) or sorted(report.get('selected_scenarios',[]))!=sorted(service):return status('stale','original full automatic selector requires full33 plus three modes')
            if group in ['wal_admission','wal_shape','wal_extent']:
                if report.get('scope')!='focused' or sorted(report.get('selected',[]))!=sorted(expected):return status('stale','focused WAL diagnostic coverage mismatch')
            if group=='cli' and (report.get('validation_only') is not True or not report.get('scenarios') or any(not row.get('pass') for row in report['scenarios'])):return status('stale','CLI fixture checks incomplete')
        return status('verified','closed PASS coupled to current sources, report bytes and required coverage; no overall shell-exit claim')
    except (OSError,ValueError,KeyError,TypeError) as error:return status('invalid','unreadable or malformed completion/evidence: '+str(error))

def export_diagnostics():
    BUILD_DIR.mkdir(parents=True,exist_ok=True)
    sources={};hashes={}
    for name in SOURCE_INPUTS:
        try:data=(ROOT/name).read_bytes();sources[name]={'present':True,'sha256':hashlib.sha256(data).hexdigest()};hashes[name]=fnv(data)
        except OSError:sources[name]={'present':False,'sha256':None};hashes[name]=None
    evidence={group:verify_group(group,hashes) for group in GROUPS}
    output=BUILD_DIR/'current-platform-diagnostics.json'
    values,errors=manifests()
    payload={'platform':'current-only',**CONTRACT,'source_provenance':{'algorithm':'SHA256','sources':sources},'completion_provenance_algorithm':FNV,'canonical_evidence':evidence,'manifest_errors':errors,'overall_gate_verified':False,'historical_evidence':'initial Red and old root-level phase1 reports retained separately; never counted as current PASS','stream_source_labels':{'os_sdk.trit':'apps/os_sdk.trit','service_runtime.trit':'apps/service_runtime.trit','embedded_guest.trit':'build_current_cleanup/service_workflow_artifact/stream/embedded_guest.trit'}}
    output.write_text(json.dumps(payload,indent=2)+'\n',encoding='utf-8');print(output);return 0

def run_test():
    if doctor() or check_contracts():return 1
    result=subprocess.run(['cmake','-S',str(ROOT),'-B',str(BUILD_DIR)],check=False)
    if result.returncode:return result.returncode
    return subprocess.run(['cmake','--build',str(BUILD_DIR),'--target','current_validate'],check=False).returncode

def main(argv=None):
    parser=argparse.ArgumentParser(prog='trit_tool.py');sub=parser.add_subparsers(dest='command',required=True)
    for command in ['doctor','contract-check','test','export-diagnostics']:sub.add_parser(command)
    args=parser.parse_args(argv)
    try:return {'doctor':doctor,'contract-check':check_contracts,'test':run_test,'export-diagnostics':export_diagnostics}[args.command]()
    except (OSError,ValueError) as error:print('current platform error: '+str(error));return 1

if __name__=='__main__':sys.exit(main())
