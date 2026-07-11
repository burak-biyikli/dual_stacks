"""Shared SE benchmark-system construction for the simulation pipeline.

The configuration is intentionally self-contained so it can be used by the
single-core and experimental multicore entrypoints without relying on a second
gem5 checkout's Python packages.
"""

import argparse

import m5
from m5.objects import (
    AddrRange,
    Cache,
    DDR4_2400_16x4,
    MemCtrl,
    Process,
    Root,
    SEWorkload,
    SrcClockDomain,
    System,
    SystemXBar,
    VoltageDomain,
    X86O3CPU,
)
from m5.objects.ValuePredictor import MemoryRenaming


BASELINE = {
    "physreg": 180,
    "float_physreg": 168,
    "lq_entries": 72,
    "sq_entries": 56,
    "load_ports": 2,
    "store_ports": 1,
}


def add_benchmark_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--mr-mode", choices=("off", "static", "full"), default="off")
    parser.add_argument("--physreg", type=int, default=BASELINE["physreg"])
    parser.add_argument("--lq-entries", type=int, default=BASELINE["lq_entries"])
    parser.add_argument("--sq-entries", type=int, default=BASELINE["sq_entries"])
    parser.add_argument("--load-ports", type=int, default=BASELINE["load_ports"])
    parser.add_argument("--store-ports", type=int, default=BASELINE["store_ports"])
    parser.add_argument("--max-insts", type=int, default=1_000_000)
    parser.add_argument("--num-cores", type=int, default=1)
    parser.add_argument("program", help="SE workload executable")
    parser.add_argument("program_args", nargs=argparse.REMAINDER)


def parse_benchmark_arguments(description: str):
    parser = argparse.ArgumentParser(description=description)
    add_benchmark_arguments(parser)
    args = parser.parse_args()
    for name in ("physreg", "lq_entries", "sq_entries", "load_ports", "store_ports", "max_insts", "num_cores"):
        if getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')} must be positive")
    return args


class L1ICache(Cache):
    size = "8KiB"
    assoc = 8
    tag_latency = 1
    data_latency = 5
    response_latency = 1
    mshrs = 8
    tgts_per_mshr = 16
    write_buffers = 8
    is_read_only = True


class L1DCache(Cache):
    size = "8KiB"
    assoc = 8
    tag_latency = 1
    data_latency = 5
    response_latency = 1
    mshrs = 16
    tgts_per_mshr = 16
    write_buffers = 64


class L2Cache(Cache):
    size = "256KiB"
    assoc = 16
    tag_latency = 4
    data_latency = 10
    response_latency = 1
    mshrs = 32
    tgts_per_mshr = 16
    write_buffers = 32


class LLCache(Cache):
    size = "16MiB"
    assoc = 16
    tag_latency = 8
    data_latency = 60
    response_latency = 1
    mshrs = 256
    tgts_per_mshr = 32
    write_buffers = 128


def configure_cpu(cpu, args) -> None:
    cpu.fetchWidth = 4
    cpu.decodeWidth = 4
    cpu.renameWidth = 8
    cpu.dispatchWidth = 8
    cpu.issueWidth = 8
    cpu.commitWidth = 8
    cpu.numROBEntries = 224
    cpu.instQueues[0].numEntries = 97
    cpu.numPhysIntRegs = args.physreg
    cpu.numPhysFloatRegs = BASELINE["float_physreg"]
    cpu.LQEntries = args.lq_entries
    cpu.SQEntries = args.sq_entries
    cpu.cacheLoadPorts = args.load_ports
    cpu.cacheStorePorts = args.store_ports
    cpu.needsTSO = True
    cpu.max_insts_any_thread = args.max_insts
    if args.mr_mode != "off":
        predictor = MemoryRenaming()
        predictor.enableStatic = True
        predictor.enableDynamic = args.mr_mode == "full"
        predictor.reallocationIsPenalty = True
        predictor.reallocationAmount = 1
        cpu.valuePred = predictor


def connect_cpu_caches(system, cpu, index: int) -> None:
    """Attach private L1/L2 caches and a shared LLC to one CPU."""
    l1i = L1ICache()
    l1d = L1DCache()
    l2 = L2Cache()
    l2bus = SystemXBar()
    setattr(system, f"l1i_{index}", l1i)
    setattr(system, f"l1d_{index}", l1d)
    setattr(system, f"l2_{index}", l2)
    setattr(system, f"l2bus_{index}", l2bus)
    cpu.icache_port = l1i.cpu_side
    cpu.dcache_port = l1d.cpu_side
    l1i.mem_side = l2bus.cpu_side_ports
    l1d.mem_side = l2bus.cpu_side_ports
    l2.cpu_side = l2bus.mem_side_ports
    l2.mem_side = system.llc_bus.cpu_side_ports


def build_system(args, cores: int = 1):
    system = System()
    system.clk_domain = SrcClockDomain(clock="4GHz", voltage_domain=VoltageDomain())
    system.mem_mode = "timing"
    system.mem_ranges = [AddrRange("8GiB")]
    system.llc_bus = SystemXBar()
    system.membus = SystemXBar()
    system.llc = LLCache()
    system.llc.cpu_side = system.llc_bus.mem_side_ports
    system.llc.mem_side = system.membus.cpu_side_ports

    cpus = []
    for index in range(cores):
        cpu = X86O3CPU(cpu_id=index, numThreads=1)
        configure_cpu(cpu, args)
        connect_cpu_caches(system, cpu, index)
        cpu.createInterruptController()
        cpu.interrupts[0].pio = system.membus.mem_side_ports
        cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
        cpu.interrupts[0].int_responder = system.membus.mem_side_ports
        cpus.append(cpu)
    system.cpu = cpus[0] if cores == 1 else cpus

    system.mem_ctrl = MemCtrl()
    system.mem_ctrl.dram = DDR4_2400_16x4()
    system.mem_ctrl.dram.range = system.mem_ranges[0]
    system.mem_ctrl.port = system.membus.mem_side_ports
    system.system_port = system.membus.cpu_side_ports
    return system, cpus


def install_workload(system, cpus, args) -> None:
    system.workload = SEWorkload.init_compatible(args.program)
    process = Process()
    process.executable = args.program
    process.cmd = [args.program] + args.program_args
    for cpu in cpus:
        cpu.workload = process
        cpu.createThreads()


def run(system) -> None:
    Root(full_system=False, system=system)
    m5.instantiate()
    print("Beginning simulation!")
    event = m5.simulate()
    print(f"Exiting @ tick {m5.curTick()} because {event.getCause()}")
