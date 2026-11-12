#!/usr/bin/env python3
"""Parse a Linux core's NT_PRSTATUS to get RIP and stack, then symbolize.

Used for the Round 75 segfault diagnosis (no gdb in the sandbox).
"""
import struct
import sys
import subprocess


def le16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def le32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def le64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def main(path):
    data = open(path, "rb").read()
    assert data[:4] == b"\x7fELF"
    # program headers
    e_phoff = le64(data, 0x20)
    e_phentsize = le16(data, 0x36)
    e_phnum = le16(data, 0x38)
    rip = rsp = None
    files = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type = le32(data, off)
        if p_type == 4:  # PT_NOTE
            p_offset = le64(data, off + 8)
            p_filesz = le64(data, off + 32)
            no = p_offset
            end = p_offset + p_filesz
            while no + 12 <= end:
                namesz = le32(data, no)
                descsz = le32(data, no + 4)
                ntype = le32(data, no + 8)
                name = data[no + 12 : no + 12 + namesz]
                desc = data[no + 12 + namesz : no + 12 + namesz + descsz]
                # align to 4
                step = 12 + ((namesz + 3) // 4) * 4 + ((descsz + 3) // 4) * 4
                no += step
                if ntype == 1 and name.startswith(b"CORE"):  # NT_PRSTATUS
                    # struct elf_prstatus: 
                    #   long flags/sig info varies... use offsets:
                    # x86_64: prstatus begins with 16*4? Let's find by
                    # scanning: RIP is at offset 112 in elf_prstatus's
                    # pr_reg (struct user_regs_struct) which itself is at
                    # offset 112 of prstatus. Actually layout:
                    # struct elf_prstatus {
                    #   struct elf_siginfo pr_info;   16 bytes (3x int + pad)
                    #   short pr_cursig;               2 (+6 pad)
                    #   unsigned long pr_sigpend;      8
                    #   unsigned long pr_sighold;      8
                    #   pid_t pr_pid..pr_gpuid;        4+4+4+4 = 16
                    #   struct timeval pr_utime..pr_stime = 48
                    #   ... total before pr_reg = 112
                    #   struct user_regs_struct pr_reg; 27*8 = 216
                    # }
                    # gregset: r15,r14,r13,r12,rbp,rbx,r11,r10,r9,r8,
                    # rax,rcx,rdx,rsi,rdi,orig_rax,RIP,cs,eflags,RSP,...
                    if len(desc) > 112 + 19 * 8:
                        rip = le64(desc, 112 + 15 * 8)
                        rsp = le64(desc, 112 + 19 * 8)
                elif ntype == 0x46494C45 and name.startswith(b"CORE"):  # NT_FILE
                    try:
                        count = le64(desc, 0)
                        base = 24
                        ents = []
                        ok = True
                        for j in range(count):
                            if base + 24 > len(desc):
                                ok = False
                                break
                            s = le64(desc, base)
                            e = le64(desc, base + 8)
                            ents.append((s, e))
                            base += 24
                        if not ok:
                            continue
                        po = 24 + count * 24 + (count + 1) * 8
                        for j in range(count):
                            if po >= len(desc):
                                break
                            endp = desc.index(b"\0", po)
                            files.append((ents[j][0], ents[j][1],
                                          desc[po:endp].decode()))
                            po = endp + 1
                    except (struct.error, ValueError):
                        pass
                if rip is not None and len(files) > 5:
                    break
        if rip is not None and len(files) > 5:
            break
    print("RIP = 0x%x  RSP = 0x%x" % (rip, rsp))
    for s, e, f in files:
        if s <= rip < e:
            print("RIP in %s (0x%x + 0x%x)" % (f, s, rip - s))
            out = subprocess.run(
                ["addr2line", "-f", "-e", f, hex(rip - s)],
                capture_output=True, text=True).stdout.strip()
            print("symbolized:", out)
            break
    # dump stack words that map to file text
    print("\nstack candidates:")
    # find PT_LOAD for stack? We'd need RSP mapping; print RSP only.
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
