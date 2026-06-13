#!/usr/bin/env python3
import argparse
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path


EGO_CONTRACT_MAGIC = 0x314F4745
EGO_FRAME_HEADER_SIZE = 72


def utc_now_iso8601():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def make_session_id():
    return "session-{}-{}".format(int(time.time() * 1000), random.randint(1000, 9999))


def parse_args():
    parser = argparse.ArgumentParser(description="Finalize board + source session on LocalPC")
    parser.add_argument("--board-base-url", required=True)
    parser.add_argument("--board-record-name", required=True)
    parser.add_argument("--source-host", required=True)
    parser.add_argument("--source-port", type=int, default=22)
    parser.add_argument("--source-user", default="admin")
    parser.add_argument("--source-password", default="")
    parser.add_argument("--source-identity-file", default="")
    parser.add_argument("--source-runtime-dir", default="~/SourceSiren/runtime")
    parser.add_argument("--source-session-id", required=True)
    parser.add_argument("--scenario-id", default="")
    parser.add_argument("--scenario-name", default="")
    parser.add_argument("--operator", default="")
    parser.add_argument("--notes", default="")
    parser.add_argument("--repeat-number", type=int, default=1)
    parser.add_argument("--siren-type", default="")
    parser.add_argument("--runtime-root", default="")
    parser.add_argument("--offline-root", default="")
    parser.add_argument("--sessions-root", default="")
    parser.add_argument("--off-config", default="")
    parser.add_argument("--off-s3-config", default="")
    parser.add_argument("--skip-s3", action="store_true")
    parser.add_argument("--source-timeout-s", type=float, default=20.0)
    parser.add_argument("--board-timeout-s", type=float, default=45.0)
    parser.add_argument("--board-state-json", default="")
    parser.add_argument("--upload-state-json", default="")
    parser.add_argument("--test-stand-config", default="board_web_localpc")
    parser.add_argument("--vehicle-id", default="BOARD-WEB-001")
    parser.add_argument("--requested-session-id", default="")
    return parser.parse_args()


def workspace_roots(args):
    script_path = Path(__file__).resolve()
    repo_root = script_path.parent.parent
    runtime_root = Path(args.runtime_root).expanduser() if args.runtime_root else repo_root / "runtimepc"
    offline_root = Path(args.offline_root).expanduser() if args.offline_root else repo_root / "offline"
    sessions_root = Path(args.sessions_root).expanduser() if args.sessions_root else runtime_root / "var" / "sessions" / "sessions"
    return repo_root, runtime_root, offline_root, sessions_root


def download_board_raw(args, raw_dir):
    os.makedirs(raw_dir, exist_ok=True)
    record_path = os.path.join(raw_dir, "board_upload_record.json")
    target_path = os.path.join(raw_dir, args.board_record_name)
    url = args.board_base_url.rstrip("/") + "/api/uploads/local?" + urllib.parse.urlencode({"name": args.board_record_name})
    with urllib.request.urlopen(url, timeout=max(args.board_timeout_s, 1.0)) as response:
        with open(target_path, "wb") as handle:
            shutil.copyfileobj(response, handle)
    with open(record_path, "w", encoding="utf-8") as handle:
        handle.write(args.upload_state_json or "{}")
    return target_path


def shell_quote(value):
    text = str(value or "")
    if not text:
        return "''"
    return "'" + text.replace("'", "'\"'\"'") + "'"


def runtime_dir_shell(path):
    text = str(path or "").strip()
    if not text:
        return "."
    if text == "~":
        return "$HOME"
    if text.startswith("~/"):
        return "$HOME/" + shell_quote(text[2:])
    return shell_quote(text)


def use_paramiko(password, identity_file):
    if password:
        try:
            import paramiko  # noqa: F401
            return True
        except ImportError:
            return False
    return bool(identity_file)


def run_ssh_command(host, port, user, password, identity_file, timeout_s, script):
    if password and not use_paramiko(password, identity_file):
        raise RuntimeError("paramiko is required on LocalPC to use password SSH towards SourceSiren")

    if use_paramiko(password, identity_file):
        return run_paramiko_command(host, port, user, password, identity_file, timeout_s, script)

    cmd = [
        "ssh",
        "-p",
        str(port),
        "-o",
        "StrictHostKeyChecking=accept-new",
    ]
    if identity_file:
        cmd += ["-i", os.path.expanduser(identity_file), "-o", "PasswordAuthentication=no"]
    cmd += ["{}@{}".format(user, host), "bash -lc {}".format(shell_quote(script))]
    return run_subprocess(cmd, timeout_s)


def run_paramiko_command(host, port, user, password, identity_file, timeout_s, script):
    try:
        import paramiko
    except ImportError as exc:
        raise RuntimeError("paramiko is required on LocalPC for password SSH") from exc

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    kwargs = {
        "hostname": host,
        "port": int(port),
        "username": user,
        "timeout": timeout_s,
        "banner_timeout": max(timeout_s, 10.0),
        "auth_timeout": max(timeout_s, 10.0),
        "allow_agent": False,
        "look_for_keys": False,
    }
    if password:
        kwargs["password"] = password
    elif identity_file:
        kwargs["key_filename"] = os.path.expanduser(identity_file)
        kwargs["allow_agent"] = True
    try:
        client.connect(**kwargs)
        _stdin, stdout, stderr = client.exec_command("bash -lc " + repr(script), timeout=timeout_s)
        out_data = stdout.read().decode("utf-8", errors="replace").strip()
        err_data = stderr.read().decode("utf-8", errors="replace").strip()
        rc = stdout.channel.recv_exit_status()
    finally:
        client.close()
    if rc != 0:
        raise RuntimeError(err_data or out_data or "remote command failed")
    return out_data


def scp_get(host, port, user, password, identity_file, timeout_s, remote_path, local_path):
    os.makedirs(os.path.dirname(local_path), exist_ok=True)
    if password and not use_paramiko(password, identity_file):
        raise RuntimeError("paramiko is required on LocalPC to use password SCP towards SourceSiren")

    if use_paramiko(password, identity_file):
        try:
            import paramiko
        except ImportError as exc:
            raise RuntimeError("paramiko is required on LocalPC for password SCP") from exc
        client = paramiko.SSHClient()
        client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        kwargs = {
            "hostname": host,
            "port": int(port),
            "username": user,
            "timeout": timeout_s,
            "banner_timeout": max(timeout_s, 10.0),
            "auth_timeout": max(timeout_s, 10.0),
            "allow_agent": False,
            "look_for_keys": False,
        }
        if password:
            kwargs["password"] = password
        elif identity_file:
            kwargs["key_filename"] = os.path.expanduser(identity_file)
            kwargs["allow_agent"] = True
        try:
            client.connect(**kwargs)
            sftp = client.open_sftp()
            try:
                sftp.get(remote_path, local_path)
            finally:
                sftp.close()
        finally:
            client.close()
        return

    cmd = [
        "scp",
        "-P",
        str(port),
        "-o",
        "StrictHostKeyChecking=accept-new",
    ]
    if identity_file:
        cmd += ["-i", os.path.expanduser(identity_file), "-o", "PasswordAuthentication=no"]
    cmd += ["{}@{}:{}".format(user, host, remote_path), local_path]
    run_subprocess(cmd, timeout_s)


def run_subprocess(cmd, timeout_s, cwd=None):
    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=max(timeout_s, 1.0),
        cwd=cwd,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError((result.stderr or result.stdout or "command failed").strip())
    return (result.stdout or "").strip()


def parse_key_values(text):
    data = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        data[key.strip()] = value.strip()
    return data


def fetch_source_bin(args, session_dir):
    export_text = run_ssh_command(
        args.source_host,
        args.source_port,
        args.source_user,
        args.source_password,
        args.source_identity_file,
        args.source_timeout_s,
        "cd {} && ./run.sh export --session-id {}".format(
            runtime_dir_shell(args.source_runtime_dir),
            shell_quote(args.source_session_id),
        ),
    )
    parsed = parse_key_values(export_text)
    remote_path = parsed.get("artifact", "").strip()
    if not remote_path:
        raise RuntimeError("SourceSiren export did not return artifact path")
    local_path = os.path.join(session_dir, "source.bin")
    scp_get(
        args.source_host,
        args.source_port,
        args.source_user,
        args.source_password,
        args.source_identity_file,
        args.source_timeout_s,
        remote_path,
        local_path,
    )
    return local_path


def build_session_metadata(session_dir, session_id, args):
    return {
        "session_id": session_id,
        "started_at_utc": utc_now_iso8601(),
        "stopped_at_utc": utc_now_iso8601(),
        "vehicle_id": args.vehicle_id,
        "test_stand_config": args.test_stand_config,
        "software_version": "localpc-finalize",
        "runtime_version": "localpc-finalize",
        "protocol_version": "1",
        "prod_protocol_version": 1,
        "source_ip": args.source_host,
        "storage_path": os.path.abspath(session_dir),
        "stop_reason": "board_source_finalize",
    }


def install_contract_bundle(session_dir, raw_log_path, session_metadata):
    dest_ego = os.path.join(session_dir, "ego.bin")
    shutil.copy2(raw_log_path, dest_ego)
    frames, t0, t1 = scan_contract_frames(dest_ego)
    manifest = {
        "session_id": str(session_metadata.get("session_id", "")),
        "first_ts_ns": t0,
        "last_ts_ns": t1,
        "packet_count": frames,
        "chunks": [
            {
                "chunk_id": 0,
                "file": "ego.bin",
                "bytes": os.path.getsize(dest_ego),
                "first_ts_ns": t0,
                "last_ts_ns": t1,
                "packet_count": frames,
            }
        ],
    }
    with open(os.path.join(session_dir, "ego_manifest.json"), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2, ensure_ascii=False)
    with open(os.path.join(session_dir, "session_metadata.json"), "w", encoding="utf-8") as handle:
        json.dump(session_metadata, handle, indent=2, ensure_ascii=False)
    return {"frames": frames, "first_ts_ns": t0, "last_ts_ns": t1}


def scan_contract_frames(path):
    count = 0
    first_t0 = 0
    last_t1 = 0
    with open(path, "rb") as handle:
        magic = struct.unpack("<I", handle.read(4))[0]
        if magic != EGO_CONTRACT_MAGIC:
            raise RuntimeError("raw log is not ego-contract EGO1")
        handle.seek(0)
        while True:
            header = handle.read(EGO_FRAME_HEADER_SIZE)
            if len(header) < EGO_FRAME_HEADER_SIZE:
                break
            payload_size = struct.unpack_from("<I", header, 56)[0]
            t0 = struct.unpack_from("<Q", header, 40)[0]
            t1 = struct.unpack_from("<Q", header, 48)[0]
            if count == 0:
                first_t0 = t0
            last_t1 = t1
            count += 1
            handle.seek(payload_size, 1)
    return count, first_t0, last_t1


def write_json(path, payload):
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, ensure_ascii=False)


def run_offline(session_dir, offline_root, args):
    cmd = [str((offline_root / "run.sh").resolve()), "process", "--session-dir", os.path.abspath(session_dir)]
    if args.off_config:
        cmd += ["--config", os.path.expanduser(args.off_config)]
    if args.off_s3_config:
        cmd += ["--s3-config", os.path.expanduser(args.off_s3_config)]
    if args.skip_s3:
        cmd += ["--skip-s3"]
    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=300,
        cwd=str(offline_root),
        check=False,
    )
    return result.returncode, (result.stdout or ""), (result.stderr or "")


def read_upload_report(session_dir):
    path = os.path.join(session_dir, "offline", "upload_report.json")
    if not os.path.isfile(path):
        return {}
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def s3_status_from(process_rc, report):
    if report.get("upload_status"):
        return str(report["upload_status"])
    if process_rc == 5:
        return "blocked"
    if process_rc == 4:
        return "failed"
    if process_rc == 0:
        return "not_run"
    return "unknown(rc={})".format(process_rc)


def main():
    args = parse_args()
    repo_root, runtime_root, offline_root, sessions_root = workspace_roots(args)
    session_id = args.requested_session_id.strip() or make_session_id()
    session_dir = os.path.join(str(sessions_root), session_id)
    raw_dir = os.path.join(session_dir, "raw")
    os.makedirs(session_dir, exist_ok=True)

    timings = {}
    t0 = time.perf_counter()
    try:
        raw_path = download_board_raw(args, raw_dir)
    except urllib.error.URLError as exc:
        print("status=error")
        print("message=board_raw_download_failed: {}".format(exc))
        return 1
    timings["download_raw_s"] = round(time.perf_counter() - t0, 3)

    t0 = time.perf_counter()
    try:
        source_path = fetch_source_bin(args, session_dir)
    except Exception as exc:
        print("status=error")
        print("message=source_download_failed: {}".format(exc))
        return 1
    timings["download_source_s"] = round(time.perf_counter() - t0, 3)

    t0 = time.perf_counter()
    session_metadata = build_session_metadata(session_dir, session_id, args)
    install_result = install_contract_bundle(session_dir, raw_path, session_metadata)
    timings["install_contract_bundle_s"] = round(time.perf_counter() - t0, 3)

    scenario_payload = {
        "scenario_id": args.scenario_id,
        "scenario_name": args.scenario_name,
        "operator": args.operator,
        "notes": args.notes,
        "repeat_number": args.repeat_number,
        "siren_type": args.siren_type,
    }
    write_json(os.path.join(session_dir, "scenario_metadata.json"), scenario_payload)
    write_json(os.path.join(session_dir, "gui_scenario_run.json"), scenario_payload)

    if args.board_state_json:
        try:
            write_json(os.path.join(session_dir, "board_session_state.json"), json.loads(args.board_state_json))
        except json.JSONDecodeError:
            pass
    if args.upload_state_json:
        try:
            write_json(os.path.join(session_dir, "board_upload_state.json"), json.loads(args.upload_state_json))
        except json.JSONDecodeError:
            pass

    t0 = time.perf_counter()
    process_rc, stdout_text, stderr_text = run_offline(session_dir, offline_root, args)
    timings["offline_process_s"] = round(time.perf_counter() - t0, 3)

    upload_report = read_upload_report(session_dir)
    s3_status = s3_status_from(process_rc, upload_report)
    report = {
        "status": "ok" if process_rc in (0, 4, 5) else "error",
        "message": "",
        "session_id": session_id,
        "session_dir": session_dir,
        "board_record_name": args.board_record_name,
        "raw_path": raw_path,
        "source_path": source_path,
        "ego_frames": install_result["frames"],
        "upload_report": upload_report,
        "upload_report_path": os.path.join(session_dir, "offline", "upload_report.json"),
        "s3_status": s3_status,
        "off_process_rc": process_rc,
        "timings": timings,
        "stdout_tail": stdout_text.splitlines()[-12:],
        "stderr_tail": stderr_text.splitlines()[-12:],
    }
    report_path = os.path.join(session_dir, "localpc_finalize_report.json")
    write_json(report_path, report)

    print("status={}".format(report["status"]))
    print("session_id={}".format(session_id))
    print("session_dir={}".format(session_dir))
    print("report_path={}".format(report_path))
    print("raw_path={}".format(raw_path))
    print("source_path={}".format(source_path))
    print("s3_status={}".format(s3_status))
    print("upload_report_path={}".format(report["upload_report_path"]))
    print("timings={}".format(json.dumps(timings, ensure_ascii=True)))
    if process_rc not in (0, 4, 5):
        print("message=offline_process_failed rc={}".format(process_rc))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
