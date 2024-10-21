#!/usr/bin/env python3

'''Test of zone transfers over TLS between Bind and Knot.'''

from dnstest.test import Test
from dnstest.utils import *
import random
import shutil
import subprocess

def upd_check_zones(master, slave, zones, prev_serials):
    for z in zones:
        master.random_ddns(z, allow_empty=False)
    serials = slave.zones_wait(zones, prev_serials)
    t.xfr_diff(master, slave, zones, prev_serials)
    return serials

t = Test(tls=True, tsig=True) # TSIG needed to skip weaker ACL rules

master = t.server("knot")
slave = t.server("bind")
zones = t.zone("example.")

t.link(zones, master, slave, ddns=True)

master.use_default_cert_key()
slave.use_default_cert_key()

t.start()

tcpdump_pcap = t.out_dir + "/traffic.pcap"
tcpdump_fout = t.out_dir + "/tcpdump.out"
tcpdump_ferr = t.out_dir + "/tcpdump.err"

tcpdump_proc = subprocess.Popen(["tcpdump", "-i", "lo", "-w", tcpdump_pcap,
                                 "port", str(master.tls_port), "or", "port", str(slave.tls_port)],
                                stdout=open(tcpdump_fout, mode="a"), stderr=open(tcpdump_ferr, mode="a"))

try:
    serials = master.zones_wait(zones)
    slave.zones_wait(zones, serials, equal=True, greater=False)
    t.xfr_diff(master, slave, zones)

    """
    master.download_cert_file(master.keydir)
    slave.gen_confile()
    slave.start()

    slave.zones_wait(zones, serials, equal=True, greater=False)
    t.xfr_diff(master, slave, zones)
    """

# NOTE: following does not work, since:
# - Knot's autogenerated self-signed certificate only contains the hostname in the CN field, for the sake of size
# - Bind doesn't accept it with conformance of RFC 8314 §8.1
    #master.cert_key_file = None
    #master.gen_confile()
    #master.stop()
    #t.sleep(2)
    #master.start()
    #master.zones_wait(zones)
    #master.cert_key_file = master.download_cert_file(master.keydir)
    #slave.gen_confile()
    #slave.reload()
    #t.sleep(3)
    #serials = upd_check_zones(master, slave, zones, serials)

finally:
    tcpdump_proc.terminate()

t.end()
