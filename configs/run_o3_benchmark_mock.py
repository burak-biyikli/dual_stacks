#!/usr/bin/env python3
"""Minimal mock benchmark config used to validate the driver argument contract."""

import argparse
import m5
from m5.objects import AddrRange, DDR3_1600_8x8, MemCtrl, Process, Root, SEWorkload, SrcClockDomain, System, SystemXBar, VoltageDomain, X86O3CPU


parser = argparse.ArgumentParser(description="Mock benchmark configuration")
parser.add_argument("--mr-mode", choices=("off", "static", "full"), default="off")
parser.add_argument("--physreg", type=int, default=180)
parser.add_argument("--lq-entries", type=int, default=72)
parser.add_argument("--sq-entries", type=int, default=56)
parser.add_argument("--load-ports", type=int, default=2)
parser.add_argument("--store-ports", type=int, default=1)
parser.add_argument("--max-insts", type=int, default=1_000_000)
parser.add_argument("--num-cores", type=int, default=1)
# MRP confidence tuning knobs (only used when --mr-mode != off)
parser.add_argument("--log-max-confidence", type=int, default=None)
parser.add_argument("--prediction-threshold", type=int, default=None)
parser.add_argument("--allocation-confidence", type=int, default=None)
parser.add_argument("--realloc-penalty", type=int, default=None)
parser.add_argument("--realloc-reinit", type=int, default=None)
parser.add_argument("--demotion-penalty", type=int, default=None)
parser.add_argument("program")
parser.add_argument("program_args", nargs=argparse.REMAINDER)
args = parser.parse_args()
print(f"Mock benchmark arguments (not applied): {vars(args)}")

system = System()
system.clk_domain = SrcClockDomain(clock="1GHz", voltage_domain=VoltageDomain())
system.mem_mode = "timing"
system.mem_ranges = [AddrRange("512MiB")]
system.cpu = X86O3CPU()
system.cpu.max_insts_any_thread = args.max_insts
system.membus = SystemXBar()
system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports
system.cpu.createInterruptController()
system.cpu.interrupts[0].pio = system.membus.mem_side_ports
system.cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
system.cpu.interrupts[0].int_responder = system.membus.mem_side_ports
system.mem_ctrl = MemCtrl(dram=DDR3_1600_8x8(), port=system.membus.mem_side_ports)
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.system_port = system.membus.cpu_side_ports
system.workload = SEWorkload.init_compatible(args.program)
process = Process(executable=args.program, cmd=[args.program] + args.program_args)
system.cpu.workload = process
system.cpu.createThreads()
Root(full_system=False, system=system)
m5.instantiate()
event = m5.simulate()
print(f"Exiting @ tick {m5.curTick()} because {event.getCause()}")
