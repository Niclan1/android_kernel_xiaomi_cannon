Cannong CPU and GPU scaling
==========================

MT6853 uses the port's ``mt6853-mcupm`` cpufreq driver, not cpufreq-dt or the
upstream MediaTek hardware-LUT driver. The latter's interface is different
from this SoC's CSRAM/mailbox protocol. The existing MT6853 driver seeds the
B24G tables/CCI map, starts MCUPM and requests an OPP index through CSRAM.
Firmware controls the PLLs and Vproc/Vsram rails. The efuse segment guard and
regulator ownership must remain intact.

The initial bring-up defaults ``ll_cap=l_cap=5`` made the five highest OPPs
boost-only and clamped firmware requests to 1.5 GHz on the six A55s and
1.837 GHz on the two A76s. Android does not enable that custom boost switch.
The retained and fresh c3y #61 6.18 logs confirm both caps; they are not
thermal throttling. Read-only c3y #61 sysfs also exposes just eleven normal
frequencies per policy, WFI/cpuoff_l and the five existing GPU frequencies.
The read-only 4.14 reference lists all sixteen OPPs, with normal maxima of
2.0 GHz and 2.4 GHz. Those are already in the port's verified B24G tables.

Both cap defaults are now zero. All sixteen stock OPPs are ordinary states,
so schedutil, cooling and the energy model see the complete range without a
userspace boost write. Explicit nonzero ``ll_cap``/``l_cap`` boot parameters
remain available for conservative bring-up. No OPP, voltage, CCI mapping or
segment selection was changed, and no overclock or thermal bypass was added.
The defconfig explicitly selects schedutil as the default governor.

Existing scheduling and idle support
-----------------------------------

Each policy shares one hardware domain (CPUs 0-5 or 6-7). The driver supports
scheduler fast-switching and remote DVFS requests; cpufreq core updates
frequency invariance. The DT supplies capacity-dmips-mhz 607/1024 and per-CPU
dynamic power coefficients. The existing EM callback now registers the full
sixteen-state range by default, rather than a truncated eleven-state range.
The coefficients are estimates, not measured per-unit power calibration.

PSCI core power-down, menu/TEO governors and the MediaTek broadcast timer are
already enabled. ``mt6853-cpuidle.dtsi`` limits the active list to ``cpuoff``
plus WFI; cluster-off remains opt-in, while mcusysoff/s2idle still require an
SPM/MCUPM low-power handshake. Do not enable those states just because they
exist in the vendor DT. Runtime usage/residency must be measured after boot.

LVTS zones retain their existing step_wise passive trips (85 C) and critical
shutdown (117 C). CPU QoS cooling limits still apply with the full table;
no trip or calibration was relaxed. Verify devinfo calibration and plausible
temperatures before sustained maximum-load testing.

Panfrost already has simple_ondemand devfreq, five OPPs (390-880 MHz), an EM
and devfreq cooling. The conservative low-OPP voltage overrides are retained.
The stock 950 MHz point remains excluded: it requires a VGPU/VSRAM coupler,
so adding only its frequency/voltage would be unsafe. No GPU change is needed
for this CPU-cap fix.

Runtime checks after an owner-authorized test
--------------------------------------------

Host checks of the compiled DT confirm the 6+2 hardware domains, capacities,
PSCI cpuoff state links, broadcast timer and five safe GPU OPPs. A table
regression matches all sixteen CPU frequencies against the 4.14 reference,
and checks monotonic EM power across the complete range. These checks do
not show actual firmware transitions, idle residency or thermal behavior.

The device has not been booted with this change. Check these read-only nodes
for both policies under ``/sys/devices/system/cpu/cpufreq/policy{0,6}``:
``scaling_driver`` (mt6853-mcupm), ``related_cpus``, ``scaling_governor``
(schedutil), ``scaling_available_frequencies`` (sixteen entries),
``cpuinfo_max_freq`` and ``scaling_max_freq`` (2000000/2400000 when cool).
There must be no old cap overrides in the boot command line. Boot logs should
show ``hybrid DVFS up: LL 500-2000 MHz, L 725-2400 MHz without boost`` and EM
registration for both domains, without MCUPM/IPI/segment errors.

Observe ``cpuinfo_cur_freq``/``scaling_cur_freq`` and time_in_state under
normal interactive use and after idle; max-frequency policy exposure alone
is not proof that firmware actually transitions. Compare cpuidle usage/time
before/after screen-off. Check thermal cooling-device states and temperature,
and the GPU devfreq governor, available_frequencies, cur_freq and trans_stat.
Do not force governors, disable thermal controls, or write policy limits on
the daily driver as part of these checks. Big cores left idle by vendor
cpuset/task-profile policy are a separate userspace issue, not missing OPPs.

Isolated build prerequisite
---------------------------

The defconfig embeds ``firmware/novatek_ts_fw.bin``. This ignored build input
is absent from a fresh Git worktree. The first isolated link attempt stopped
with "No rule to make target .../firmware/novatek_ts_fw.bin"; no C compiler
error preceded it. Restore the existing device firmware into the isolated
worktree, without changing config or the main worktree. The reference is
118784 bytes with SHA-256
``1f5f956304fcf214543648b2178732fdcba7d1cf39ef36c45834004be3743b75``.
It is not added to Git by these changes. Build with the existing clang-r596125
recipe, ``-j4`` and a separate output directory; do not reuse the main output
or clean compiler caches.
