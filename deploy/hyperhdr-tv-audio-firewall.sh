#!/bin/bash
# Restrict the HyperHDR audio UDP ports to permitted senders. Idempotent.
set -e

# Port 17000: techno-engine audio input. Allowed senders: loopback (the local
# tv-audio-adapter) and the legacy Mac sender.
PORT=17000
for src in 127.0.0.1/32 10.10.40.106/32; do
  while iptables -C INPUT -p udp --dport "$PORT" -s "$src" -j ACCEPT 2>/dev/null; do
    iptables -D INPUT -p udp --dport "$PORT" -s "$src" -j ACCEPT
  done
done
while iptables -C INPUT -p udp --dport "$PORT" -j DROP 2>/dev/null; do
  iptables -D INPUT -p udp --dport "$PORT" -j DROP
done
iptables -I INPUT 1 -p udp --dport "$PORT" -s 127.0.0.1/32 -j ACCEPT
iptables -I INPUT 2 -p udp --dport "$PORT" -s 10.10.40.106/32 -j ACCEPT
iptables -I INPUT 3 -p udp --dport "$PORT" -j DROP

# Port 5004: webOS TV RTP audio input to the adapter. Allowed sender: TV only.
PORT2=5004
TV_IP=10.10.40.104
while iptables -C INPUT -p udp --dport "$PORT2" -s "$TV_IP/32" -j ACCEPT 2>/dev/null; do
  iptables -D INPUT -p udp --dport "$PORT2" -s "$TV_IP/32" -j ACCEPT
done
while iptables -C INPUT -p udp --dport "$PORT2" -j DROP 2>/dev/null; do
  iptables -D INPUT -p udp --dport "$PORT2" -j DROP
done
iptables -I INPUT 1 -p udp --dport "$PORT2" -s "$TV_IP/32" -j ACCEPT
iptables -I INPUT 2 -p udp --dport "$PORT2" -j DROP
