#!/usr/bin/env python3
"""ircd_rig.py — the miniature IRC server for the fdk-chat rigs.

Speaks enough RFC 2812 for the client: NICK/USER handshake with the
welcome numerics, JOIN (echo + topic + NAMES + end-of-names), a
styled PRIVMSG, a highlight PRIVMSG (mentions the client's nick),
an optional STORM (N fast PRIVMSGs — the streaming-append stress),
PING/PONG, PART/QUIT echo. One channel per connection.

Usage: ircd_rig.py <port> <storm_count>
"""
import socket
import sys
import threading

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 16667
STORM = int(sys.argv[2]) if len(sys.argv) > 2 else 0
LOG = open("/tmp/fdk-chat-ircd.log", "w")


def send(f, line):
    f.write((line + "\r\n").encode("utf-8"))
    LOG.write(">> " + line + "\n")
    LOG.flush()


def client_thread(conn, addr):
    LOG.write("== connection from %s\n" % (addr,))
    LOG.flush()
    f = conn.makefile("rwb")
    nick = "anon"
    joined = None
    try:
        for raw in f:
            try:
                line = raw.decode("utf-8", "replace").strip()
            except Exception:
                continue
            if not line:
                continue
            LOG.write("<< " + line + "\n")
            LOG.flush()
            parts = line.split(" ", 1)
            cmd = parts[0].upper()
            rest = parts[1] if len(parts) > 1 else ""
            if cmd == "NICK":
                nick = rest.split(" ")[0].lstrip(":")
            elif cmd == "USER":
                send(f, ":irc.rig 001 %s :Welcome to the FDK "
                        "chat rig" % nick)
                send(f, ":irc.rig 002 %s :Your host is irc.rig"
                        % nick)
                send(f, ":irc.rig 003 %s :This server was created "
                        "for the fdk-chat round" % nick)
                send(f, ":irc.rig 004 %s irc.rig 1.0 iowx :" % nick)
                send(f, ":irc.rig 251 %s :There are 1 users and 0 "
                        "services on 1 servers" % nick)
            elif cmd == "PING":
                payload = rest[1:] if rest.startswith(":") else rest
                send(f, ":irc.rig PONG irc.rig :%s" % payload)
            elif cmd == "JOIN" and joined is None:
                joined = rest.split(" ")[0].lstrip(":")
                send(f, ":%s!u@h JOIN %s" % (nick, joined))
                send(f, ":irc.rig 332 %s %s :The FDK chat rig "
                        "topic" % (nick, joined))
                send(f, ":irc.rig 353 %s = %s :@alice +bob carol "
                        "dave %s" % (nick, joined, nick))
                send(f, ":irc.rig 366 %s %s :End of /NAMES list"
                        % (nick, joined))
                # a styled message: bold nick, a URL, fg+bg colors
                send(f, ":alice!a@h PRIVMSG %s :hello \x02%s\x0f, "
                        "see https://example.com/x and \x0312,04"
                        "styled\x0f" % (joined, nick))
                # a highlight: mentions the client's nick
                send(f, ":bob!b@h PRIVMSG %s :hey %s, look at "
                        "this" % (joined, nick))
                # a private message (creates its own conversation)
                send(f, ":carol!c@h PRIVMSG %s :psst, private "
                        "hello" % nick)
                if STORM > 0:
                    for i in range(STORM):
                        send(f, ":storm%d!s@h PRIVMSG %s :storm "
                                "message %d \x03%dcolored\x0f "
                                "tail https://storm.example/%d"
                                % (i % 7, joined, i, i % 16, i))
                        if i % 250 == 0:
                            f.flush()
                f.flush()
            elif cmd == "PART":
                send(f, ":%s!u@h PART %s :leaving"
                     % (nick, rest.split(" ")[0]))
            elif cmd == "QUIT":
                send(f, "ERROR :Closing Link: %s (Quit)"
                     % nick)
                break
    except (ConnectionResetError, BrokenPipeError, OSError):
        pass
    finally:
        LOG.write("== closed %s\n" % (addr,))
        LOG.flush()
        try:
            conn.close()
        except OSError:
            pass


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT))
    srv.listen(4)
    LOG.write("== ircd rig on %d (storm=%d)\n" % (PORT, STORM))
    LOG.flush()
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=client_thread, args=(conn, addr),
                         daemon=True).start()


if __name__ == "__main__":
    main()
