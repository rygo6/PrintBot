import json
from pathlib import Path
import selectors
import subprocess
import uuid

root = Path(__file__).resolve().parents[2]
p = subprocess.Popen([str(root / 'print_bot-mcp.sh'), '--gui'], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
seq = 0

def rpc(method, params):
    global seq
    seq += 1
    p.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': seq, 'method': method, 'params': params}) + '\n')
    p.stdin.flush()
    with selectors.DefaultSelector() as selector:
        selector.register(p.stdout, selectors.EVENT_READ)
        while selector.select(45):
            line = p.stdout.readline()
            if not line:
                raise RuntimeError('MCP disconnected')
            response = json.loads(line)
            if response.get('id') == seq:
                assert 'error' not in response, response
                return response['result']
    raise TimeoutError('MCP did not respond; no retry is permitted')

def call(name, arguments=None):
    return rpc('tools/call', {'name': name, 'arguments': arguments or {}})

def success(name, arguments=None):
    result = call(name, arguments)
    assert not result.get('isError'), result
    return result['structuredContent']

def error(arguments, code):
    result = call('print_plate', arguments)
    assert result.get('isError') and result['structuredContent']['error']['code'] == code, result

try:
    init = rpc('initialize', {'protocolVersion': '2024-11-05', 'capabilities': {},
                             'clientInfo': {'name': 'print-authorization-test', 'version': '1'}})
    assert 'Do not ask again' in init['instructions']
    p.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
    p.stdin.flush()
    status = success('get_status')
    assert status.get('test_no_send') is True, 'Requires PRINTBOT_MCP_TEST_NO_SEND=1; refusing send tests'
    assert status['printer_control_allowed'] is True
    assert status['current_plate']['slice']['sliced'] is True, 'Prepare a sliced test plate first'
    schema = next(t for t in rpc('tools/list', {})['tools'] if t['name'] == 'print_plate')['inputSchema']['properties']
    assert 'user_authorized' in schema and 'authorization_id' in schema
    assert 'approval_timeout_s' not in schema
    pre = success('print_plate')
    request_id = 'test-' + str(uuid.uuid4())
    args = {'dry_run': False, 'fingerprint': pre['job_fingerprint'],
            'user_authorized': True, 'authorization_id': request_id}
    error({**args, 'user_authorized': False}, 'print.not_authorized')
    error({**args, 'authorization_id': '../invalid'}, 'invalid_argument')
    error({**args, 'fingerprint': 'stale'}, 'approval.stale')
    result = success('print_plate', args)
    assert result['status'] == 'test_no_send', result
    assert result['authorization_id'] == request_id
    assert success('get_print_operation', {'operation_id': result['operation_id']})['status'] == 'test_no_send'
    error(args, 'approval.used')
    state = success('ui_get_state')
    assert all(d['class'] != 'PrintBotApproval' for d in state['dialogs'])
    print('PASS: chat authorization contract, missing approval, invalid ID, stale job, no-popup test send, replay rejection')
finally:
    p.terminate()
    p.wait(timeout=10)
