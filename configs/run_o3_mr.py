import m5
from m5.objects import *
import os
import sys

# Import Value Predictors
from m5.objects.ValuePredictor import MemoryRenaming

system = System()
system.clk_domain = SrcClockDomain()
system.clk_domain.clock = "1GHz"
system.clk_domain.voltage_domain = VoltageDomain()
system.mem_mode = "timing"
system.mem_ranges = [AddrRange("512MiB")]

# Use O3 CPU
system.cpu = X86O3CPU()
system.cpu.mock_rdtsc = True

# Attach Memory Renaming logic
system.cpu.valuePred = MemoryRenaming()

# --- Memory Renaming Configuration Stubs ---
# You can override the default predictor parameters here:
# system.cpu.valuePred.ways = 4
# system.cpu.valuePred.tagWidth = 16
# system.cpu.valuePred.logESTBEntrys = 10
# system.cpu.valuePred.logMaxConfidence = 2
# system.cpu.valuePred.predictionThreshold = 3
# system.cpu.valuePred.allocationConfidence = 1
# system.cpu.valuePred.reallocationIsPenalty = True
# system.cpu.valuePred.reallocationAmount = 1
# system.cpu.valuePred.demotionPenalty = 4
# system.cpu.valuePred.mrtQueueCapacity = 8
# system.cpu.valuePred.enableDynamic = True
# system.cpu.valuePred.enableStatic = True



system.membus = SystemXBar()
system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports

system.cpu.createInterruptController()
system.cpu.interrupts[0].pio = system.membus.mem_side_ports
system.cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
system.cpu.interrupts[0].int_responder = system.membus.mem_side_ports

system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR3_1600_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports

system.system_port = system.membus.cpu_side_ports

# Parse arguments
import argparse
parser = argparse.ArgumentParser()
parser.add_argument("--max-insts", type=int, default=100000, help="Max instructions")
parser.add_argument("--realloc-mode", type=str, choices=["reinit", "penalty"], default="penalty", help="Reallocation mode on producer change")
parser.add_argument("--realloc-amount", type=int, default=1, help="Reallocation penalty or initialization amount")
args, remainder = parser.parse_known_args()

if not remainder:
    print("Error: No binary specified")
    sys.exit(1)

args.binary = remainder[0]
args.benchmark_args = remainder[1:]

system.cpu.max_insts_any_thread = args.max_insts
system.cpu.valuePred.reallocationIsPenalty = (args.realloc_mode == "penalty")
system.cpu.valuePred.reallocationAmount = args.realloc_amount

system.workload = SEWorkload.init_compatible(args.binary)
process = Process()
process.cmd = [args.binary] + args.benchmark_args
system.cpu.workload = process
system.cpu.createThreads()

root = Root(full_system=False, system=system)
m5.instantiate()
print("Beginning simulation!")
exit_event = m5.simulate()
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
