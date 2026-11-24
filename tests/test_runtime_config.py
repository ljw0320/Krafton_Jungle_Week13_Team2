"""Fail-fast checks for environment capacities and inherited FD budgets."""
import os
from pathlib import Path
import resource
import socket
import subprocess
ROOT = Path(__file__).resolve().parents[1]
def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0)); return str(s.getsockname()[1])

def check(program, settings, expected, low_fd=False):
    env = dict(os.environ)
    for name in ('JUNGLE_CONNECTIONS','JUNGLE_CONTEXTS','PROXY_CONNECTIONS','PROXY_CONTEXTS'):
        env.pop(name, None)
    env.update(settings)
    command = ['./reverse_proxy', port(), '127.0.0.1', port()] if program == 'proxy' else ['./webserver', port(), 'www']
    def limit():
        _, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        resource.setrlimit(resource.RLIMIT_NOFILE, (64, hard))
    result = subprocess.run(command, cwd=str(ROOT), env=env, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, timeout=3, preexec_fn=limit if low_fd else None)
    assert result.returncode != 0
    assert expected.encode() in result.stderr, result.stderr

for program, prefix, field in [('proxy','PROXY','CONTEXTS'),('server','JUNGLE','CONTEXTS')]:
    for value in ('0','-1','abc','65537'):
        check(program, {prefix+'_CONNECTIONS': value}, prefix+'_CONNECTIONS must be')
    check(program, {prefix+'_CONNECTIONS':'2', prefix+'_'+field:'3'}, 'must not exceed')
    check(program, {prefix+'_CONNECTIONS':'128'}, 'FD limit too low', True)
print('runtime configuration: 12 invalid-value/relation/FD-budget checks passed')
