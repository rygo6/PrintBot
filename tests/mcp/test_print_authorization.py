from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
scratch = root / 'temp'
scratch.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='print-authorization-', dir=scratch) as tmp:
    tmp = Path(tmp)
    source = tmp / 'probe.cpp'
    source.write_text('''#include "mcp/PrintAuthorization.hpp"
int main(int argc, char **argv)
{
    if (argc != 3) return 99;
    return int(printbot::mcp::consume_print_authorization(argv[1], argv[2]));
}
''')
    exe = tmp / 'probe'
    subprocess.run(['c++', '-std=c++17', '-I', str(root / 'src'), str(source), '-o', str(exe)], check=True)
    ledger = tmp / 'ledger'

    def consume(identifier, directory=ledger):
        return subprocess.run([str(exe), str(directory), identifier], timeout=10).returncode

    assert consume('chat-message-1') == 0
    assert consume('chat-message-1') == 1
    assert consume('chat-message-2') == 0
    assert consume('chat-message-1') == 1
    for identifier in ['', '../escape', 'a/b', 'a\\b', 'x' * 129, 'a b', 'é']:
        assert consume(identifier) == 2, identifier
    assert consume('x' * 128) == 0
    assert len(list(ledger.iterdir())) == 3
    with ThreadPoolExecutor(max_workers=16) as pool:
        results = list(pool.map(lambda _: consume('concurrent-request'), range(32)))
    assert results.count(0) == 1, results
    assert results.count(1) == 31, results
    blocked = tmp / 'file-instead-of-directory'
    blocked.write_text('unavailable')
    assert consume('blocked-request', blocked) == 3
    assert consume('concurrent-request') == 1
    print('PASS: persistence across processes, 32 concurrent attempts, distinct requests, invalid IDs, failed ledger writes')
