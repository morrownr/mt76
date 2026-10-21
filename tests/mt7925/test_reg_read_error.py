#!/usr/bin/env python3
"""Exercise the real debugfs getter against MCU success and failure replies."""
from pathlib import Path
import subprocess
import tempfile

import unittest

def run_fixture(root=None):
    root = Path(root) if root is not None else Path(__file__).resolve().parents[2]
    source = (root/'mt7925/debugfs.c').read_text()
    start = source.index('static int\nmt7925_reg_get(')
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    getter = source[start:end]

    harness = r'''
    #include <stdint.h>
    #include <stdio.h>
    #include <stdbool.h>
    #include <errno.h>
    typedef uint32_t u32;
    typedef uint64_t u64;
    struct mt792x_dev { struct { u32 debugfs_reg; } mt76; };
    static int reply, locks, calls, violations;
    static u32 register_value;
    static void mt792x_mutex_acquire(struct mt792x_dev *d) { (void)d; locks++; }
    static void mt792x_mutex_release(struct mt792x_dev *d) { (void)d; locks--; }
    static int mt7925_mcu_regval(struct mt792x_dev *d, u32 address,
                               u32 *value, bool write)
    {
        calls++;
        violations += locks != 1 || write || address != d->mt76.debugfs_reg;
        if (!reply) *value = register_value;
        return reply;
    }
    '''
    cases = r'''
    int main(void)
    {
        struct mt792x_dev dev = { .mt76.debugfs_reg = 0x820d4370 };
        const int replies[] = {0, -ETIMEDOUT, -EIO, -EHOSTDOWN};
        int failures = 0, checks = 0;
        for (unsigned i = 0; i < sizeof(replies)/sizeof(replies[0]); i++) {
            u64 value = 0xabcddcba12344321ULL;
            reply = replies[i]; register_value = 0xc001d00d;
            int status = mt7925_reg_get(&dev, &value);
            checks++; failures += status != reply;
            if (status != reply)
                printf("reply=%d getter returned=%d instead of the MCU error\n", reply, status);
            checks++; failures += value != (reply ? 0xabcddcba12344321ULL : register_value);
            checks++; failures += locks != 0 || calls != (int)i + 1 || violations;
        }
        printf("%d checks, %d failures\n", checks, failures);
        return failures != 0;
    }
    '''
    with tempfile.TemporaryDirectory(prefix='mlo-reg-read-') as task_dir:
        c = Path(task_dir)/'test.c'
        binary = Path(task_dir)/'test'
        c.write_text(harness+getter+cases)
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Werror',str(c),'-o',str(binary)],check=True)
        result = subprocess.run([str(binary)])
    result.check_returncode()


class ProductionFixture(unittest.TestCase):
    def test_register_error_propagation(self):
        run_fixture()


if __name__ == "__main__":
    unittest.main()
