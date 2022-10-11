#!/usr/bin/env python2
from __future__ import print_function
from distutils.command.build import build
import json
import time
import sys
import os


def prGreen(skk):
    print("\033[92m %s \033[00m" % skk)
    sys.stdout.flush()


def run_cmd(cmd):
    prGreen(cmd)
    os.system(cmd)
    return

def generate_pssh_hoststr(hostlist):
    hoststr = '\"'
    hostlist = list(set(hostlist))
    for i in hostlist:
        hoststr = hoststr + i + ' '
    hoststr = hoststr.strip(' ')
    hoststr = hoststr + '\"'
    return hoststr

master = "10.0.2.170"
servers = ["10.0.2.160"]
clients = ["10.0.2.161"]

src_path = "/root/rlibv2"
build_path = src_path + "/build"

def init():
    run_cmd("parallel-ssh -H %s -i \"mkdir %s\"" %
            (generate_pssh_hoststr(servers + clients), src_path))
    run_cmd("parallel-ssh -H %s -i \"mount %s:%s %s\"" %
            (generate_pssh_hoststr(servers + clients), master, src_path, src_path))

def compile():
    run_cmd("mkdir %s; cd %s; cmake ..; make -j" %
            (build_path, build_path))

if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "init":
        init()
    if cmd == "compile":
        compile()
