// Finite source-owned native import roles. Case input never supplies a path or executable.
export const NATIVE_IMPORT_PACKAGES = {
  "native": "gateway-native",
  "flow": "gateway-native-import-fixture-tests",
  "custody": "gateway-native-custody-wire",
  "tools": "gateway-native-import-tools"
} as const;
export type NativeImportPackage = keyof typeof NATIVE_IMPORT_PACKAGES;
export const NATIVE_IMPORT_ROLES = [
  { tag: "import_flow_positive", package: "flow", kind: "test", name: "import_flow_positive", source: "tests/import_flow_positive.rs" },
  { tag: "import_flow_negative", package: "flow", kind: "test", name: "import_flow_negative", source: "tests/import_flow_negative.rs" },
  { tag: "import_owned_children", package: "flow", kind: "test", name: "import_owned_children", source: "tests/import_owned_children.rs" },
  { tag: "import_policy", package: "native", kind: "test", name: "import_policy", source: "tests/import_policy.rs" },
  { tag: "custody_identity", package: "custody", kind: "test", name: "custody_identity", source: "tests/custody_identity.rs" },
  { tag: "sandbox_identity", package: "tools", kind: "lib", name: "gateway_native_import_tools", source: "src/lib.rs" },
 ] as const;
export type NativeImportRole = (typeof NATIVE_IMPORT_ROLES)[number]["tag"];
export const requireNativeImportRole = (tag: string) => {
  const role = NATIVE_IMPORT_ROLES.find((item) => item.tag === tag);
  if (!role) throw new Error("Unknown fixed native import role");
  return role;
};
export const NATIVE_IMPORT_CHILDREN: Readonly<Record<NativeImportRole, readonly string[]>> = {
  "import_flow_positive": [
    "existing_account_import::flow_tests::controlled_custodian_process",
    "existing_account_import::foundation_tests::controlled_async_writer_child"
  ],
  "import_flow_negative": [
    "existing_account_import::flow_tests::controlled_custodian_process",
    "existing_account_import::foundation_tests::controlled_async_writer_child"
  ],
  "import_owned_children": [
    "existing_account_import::foundation_tests::controlled_async_writer_child"
  ],
  "import_policy": [],
  "custody_identity": [],
  "sandbox_identity": []
};
export const NATIVE_IMPORT_CASES = [
  {
    name: "existing_account_import::flow_tests::actual_existing_account_original_start_owned_child_custody_durability_and_publication",
    role: "import_flow_positive",
    boundary: "prelaunch",
    seconds: 90,
  },
  {
    name: "existing_account_import::flow_tests::negatives::actual_quiet_client_eof_before_and_after_started_never_writes_or_publishes",
    role: "import_flow_negative",
    boundary: "prelaunch",
    seconds: 90,
  },
  {
    name: "existing_account_import::flow_tests::negatives::actual_custodian_death_after_stop_closes_live_actor_without_late_effect",
    role: "import_flow_negative",
    boundary: "prelaunch",
    seconds: 90,
  },
  {
    name: "existing_account_import::flow_tests::negatives::actual_admin_revocation_before_native_write_refuses_the_same_started_claim",
    role: "import_flow_negative",
    boundary: "prelaunch",
    seconds: 90,
  },
  {
    name: "existing_account_import::flow_tests::negatives::actual_original_deadline_expiry_after_stop_never_renews_mutation_authority",
    role: "import_flow_negative",
    boundary: "prelaunch",
    seconds: 90,
  },
  {
    name: "existing_account_import::flow_tests::negatives::actual_revoked_publication_retains_native_receipt_but_never_source_success",
    role: "import_flow_negative",
    boundary: "prelaunch",
    seconds: 90,
  },
  {
    name: "existing_account_import::flow_tests::negatives::actual_lost_publication_reply_reconciles_only_the_same_native_receipt",
    role: "import_flow_negative",
    boundary: "prelaunch",
    seconds: 90,
  },
  {
    name: "existing_account_import::flow_tests::negatives::fixture_enabled_ordinary_entrypoints_refuse_before_reading_stdin",
    role: "import_flow_negative",
    boundary: "loopback",
    seconds: 20,
  },
  {
    name: "actual_import_profile_exact_names_fixed_ports_process_and_sibling_controls",
    role: "import_policy",
    boundary: "prelaunch",
    seconds: 30,
  },
  {
    name: "stopped_replay::raw_stopped_replay_wrong_session_sequence_challenge_and_binding_are_not_live_custody",
    role: "custody_identity",
    boundary: "loopback",
    seconds: 15,
  },
  {
    name: "a_stopped_session_rejects_new_kernel_epoch_or_root_identity",
    role: "custody_identity",
    boundary: "offline",
    seconds: 10,
  },
  {
    name: "existing_account_import::foundation_tests::actual_owned_async_child_stop_wait_and_dirty_epoch_do_not_infer_external_absence",
    role: "import_owned_children",
    boundary: "prelaunch",
    seconds: 20,
  },
  {
    name: "existing_account_import::foundation_tests::actual_owned_child_reap_records_stopped_but_reopened_marker_grants_no_lease",
    role: "import_owned_children",
    boundary: "prelaunch",
    seconds: 20,
  },
  {
    name: "existing_account_import::flow_tests::fixture_child_paths_refuse_escaping_and_symlinked_bindings",
    role: "import_flow_positive",
    boundary: "offline",
    seconds: 10,
  },
  {
    name: "sandbox_identity::fixture_sandbox_keeps_two_exact_identities_and_full_fixed_executable_verification",
    role: "sandbox_identity",
    boundary: "offline",
    seconds: 10,
  },
  {
    name: "owner_file_policy::prepared_files_require_private_owned_regular_single_link_data",
    role: "custody_identity",
    boundary: "offline",
    seconds: 10,
  },
  {
    name: "owner_file_policy::stopped_auth_retains_original_permission_and_size_domain",
    role: "custody_identity",
    boundary: "offline",
    seconds: 10,
  },
  {
    name: "owner_file_policy::generation_size_boundary_does_not_add_a_private_mode_check",
    role: "custody_identity",
    boundary: "offline",
    seconds: 10,
  },
  {
    name: "owner_file_policy::epoch_shape_boundary_leaves_parent_uid_observation_to_b",
    role: "custody_identity",
    boundary: "offline",
    seconds: 10,
  },
] as const;
export const NATIVE_IMPORT_PROCESS = String.raw`import subprocess,json,os,signal,time,sys,tempfile,re
payload=json.loads(sys.argv[1]);receipt={};p=None;owned={};interrupted=None
def snapshot():
 rows={}
 for line in subprocess.check_output(['/bin/ps','-axo','pid=,ppid=,pgid=,lstart=,command='],text=True).splitlines():
  parts=line.strip().split(None,8)
  if len(parts)==9:rows[int(parts[0])]={'ppid':int(parts[1]),'pgid':int(parts[2]),'started':' '.join(parts[3:8]),'command':parts[8]}
 return rows
def observe():
 rows=snapshot()
 if p is not None and p.poll() is None and p.pid in rows and p.pid not in owned:owned[p.pid]=rows[p.pid]
 changed=True
 while changed:
  changed=False
  for pid,row in rows.items():
   parent=rows.get(row['ppid']);prior=owned.get(row['ppid'])
   if pid not in owned and parent is not None and prior is not None and parent['started']==prior['started'] and parent['pgid']==prior['pgid']:
    owned[pid]=row;changed=True
 return {pid:rows[pid] for pid,prior in owned.items() if pid in rows and rows[pid]['started']==prior['started'] and rows[pid]['pgid']==prior['pgid']}
def stop_groups(current,sig):
 for group in sorted({row['pgid'] for row in current.values()}):
  try:os.killpg(group,sig)
  except ProcessLookupError:pass
  receipt.setdefault('settlement_signals',[]).append({'group':group,'signal':sig})
def settle():
 if p is None:return
 current=observe()
 if current:stop_groups(current,signal.SIGTERM)
 until=time.monotonic()+3
 while current and time.monotonic()<until:
  p.poll();time.sleep(.05);current=observe()
 if current:stop_groups(current,signal.SIGKILL)
 if p.poll() is None:p.wait(timeout=5)
 until=time.monotonic()+2
 current=observe()
 while current and time.monotonic()<until:
  time.sleep(.05);current=observe()
 receipt['owned_process_observations']=owned;receipt['owned_processes_remaining']=current
 if current:raise RuntimeError('Owned task processes did not settle: '+repr(current))
def interrupt(signum,_frame):
 global interrupted
 interrupted=signum
 raise InterruptedError('Native flow driver received signal '+str(signum))
old_handlers={sig:signal.signal(sig,interrupt) for sig in [signal.SIGINT,signal.SIGTERM]}
output=tempfile.TemporaryFile()
try:
 p=subprocess.Popen(payload['argv'],cwd=payload['cwd'],env=payload['environment'],stdout=output,stderr=subprocess.STDOUT,start_new_session=True)
 deadline=time.monotonic()+payload['seconds']
 while p.poll() is None:
  observe()
  if time.monotonic()>=deadline:raise TimeoutError('Exact native import case deadline')
  time.sleep(.1)
 output.seek(0);text=output.read().decode();print(text,flush=True)
 results=re.findall(r'test result: (.+)',text)
 assert p.returncode==0 and len(results)==1 and results[0].startswith('ok. 1 passed; 0 failed; 0 ignored;'), 'Exact native import case failed'
 receipt['status']='passed'
except BaseException as error:receipt['status']='failed';receipt['error']=repr(error);raise
finally:
 for sig in old_handlers:signal.signal(sig,signal.SIG_IGN)
 try:settle()
 except BaseException as error:receipt['status']='failed';receipt['settlement_error']=repr(error)
 output.seek(0)
 if receipt.get('status')!='passed':print(output.read().decode(),flush=True)
 output.close();receipt['exit']=None if p is None else p.returncode;receipt['interrupted_signal']=interrupted
 if interrupted is not None:receipt['status']='failed'
 print(json.dumps({'nativeImportProcess':receipt}),flush=True)
 if receipt.get('status')!='passed' and 'error' not in receipt:raise RuntimeError('Owned native import cleanup failed')
`;
