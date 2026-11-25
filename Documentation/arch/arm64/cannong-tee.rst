Cannong Microtrust boot ordering
===============================

The GP client device is not ready merely because CAPI shared memory exists.
On the previous 6.18 build, ``teei_flags`` became 1 before the daemon's driver
TA list was loaded, even if framework initialization failed. The session retry
loop stopped as soon as that flag became 1. This left a second race after the
original ten-second retry window, and a missing keymaster TA (``ffff0008``)
could abort the HAL while init was blocked in ``mount_all --late``.

Readiness now has two stages. The transport-ready flag permits the in-kernel
TA loader to use CAPI. The GP-ready flag and fingerprint wakeup are published
only after the entire requested TA list has been attempted. The vendor list
is best-effort: it contains UUIDs absent on this product. A failed optional
preload must not prevent later entries from loading. INIT is serialized and
its result is cached; a partial secure-world initialization is never replayed.
Framework errors wake client waiters with an error. Readiness is an ordering
barrier, not a claim that every optional app exists: each client's own session
still returns the secure-world result, including authentication failures.
Only userspace ``/dev/isee_tee*`` open waits for the completion (interruptible,
60-second failure timeout). ``tee_k_open()``, the VFS/RPMB daemon and the legacy
keymaster/UNLOCK device stay outside that gate. No CPU-hotplug lock is held
while a client waits. The old session polling/retry loop is removed.

Required vendor patch
---------------------

Do not deploy the gate alone. The delivered ``patches/tee`` directory has
both a guarded vendor patch and a reversible boot-ramdisk overlay. Both
require the matching boot-image opt-in::

    androidboot.cannon.tee=fbe-gate

The old unconditional ``on fs`` vendor override is superseded. The replacement
vendor action is early-init, requires ``ro.vendor.cannon.kernel=mainline``
and the explicit opt-in, and leaves 4.14 startup unchanged. This option needs
the updated vendor init file and the existing libinit kernel-flavor property.

For testing without a vendor rebuild, inject ``boot-overlay/overlay.d`` into
the Magisk-patched boot ramdisk using Magisk's root-directory overlay facility.
It is not a normal module. The synchronous early-init helper checks all of:
Linux 6.18, MT6853 cannong/cannon, physical UFS RPMB, the explicit opt-in,
the exact kernel gate capability
``/sys/class/teei_config/teei_config/boot_gate`` equal to 1, and an FBE userdata
entry with the metadata key directory in the mounted vendor fstab. Only then
does it set the daemon's existing override before ``fs`` starts the daemon::

    setprop vendor.soter.teei.crypto.state unencrypted

The helper leaves 4.14, unpaired kernels, and non-FBE configurations untouched.
Restore the original boot image to remove the test overlay and opt-in. The
owner must prepare and inspect the paired image before booting it; no
partitions are modified by the helper. Normal Magisk ``system.prop``, vendor
file mounts, ``post-fs-data.sh`` and ``service.sh`` are too late: init has
already parsed vendor rc files, and late-fs can already be waiting for
keymaster. Do not advertise one of those as an early-init fix.

This does NOT change ``ro.crypto.state``, ``ro.crypto.type``, ``vold.decrypt``,
userdata or disk-encryption policy. It releases the daemon's obsolete FDE
handshake. No TAs, keys, RPMB contents, security partitions or BCB are changed
by this code. Do not copy it to an FDE or virtual-RPMB product.

The vendor daemon has independent INIT and UNLOCK threads. Disassembly of the
current binary shows that UNLOCK proceeds when either the override is
``unencrypted`` or ``ro.crypto.type`` is ``file``. Waiting for the latter is
not sufficient: the property may only appear during late-fs, after vold has
already requested keymaster. Nor may UNLOCK depend on ``post-fs-data``,
``vold.post_fs_data_done``, ``vendor.soter.teei.init=INIT_OK``, or a successful
keymaster open. That would recreate the earlier completion-gate deadlock.

The current vendor VFS library loads TAs from ``/vendor/thh/``. RPMB is physical
UFS, selected through the existing BOOT ``boot_type`` compatibility interface;
neither needs decrypted /data. The optional LOG.ini thread may still wait
for /data. Keep all existing TEE device permissions and daemon startup in fs;
do not move the daemon to class late_start or gate its config/VFS opens.

Evidence and validation
-----------------------

* Checkpoint 183 records the earlier gate deadlock and ITEM_NOT_FOUND retry.
* The retained c3y-v8 6.18 log shows CAPI at 4.276 s, UNLOCK at 4.847 s,
  ``TEEI start to load driver TAs`` at 4.847 s, and the end at 5.142 s.
  The old readiness flag was already true throughout that last interval.
* The newly collected c3y #61 6.18 boot independently shows the same gap:
  Keymaster Unlocked at 4.210 s, UNLOCK at 4.791 s, and driver TA loading
  from 4.791 through 5.106 s. This successful boot does not rule out the race.
* The first live reference was 4.14. A later owner booted c3y #61, not the
  modified kernel. Neither is runtime validation of this change.
* Ten ASan/UBSan host tests compile the actual init/gate and loader helpers
  with a pthread/mock backend. The stock 23-entry daemon list exercises first,
  last and multiple optional failures, competing INITs, independent UNLOCK,
  no early GP release, framework failure, loader result mapping, invalid
  UUID/count, timeout and interruption. Run from the kernel root::

    python3 tools/testing/selftests/teei/boot_gate_test.py

  Generated files stay in ``.teei-boot-tests`` (or ``--output-dir``).
  These tests verify ordering/error propagation, not SMCs or secure firmware.
* Source/host control-flow tests and an isolated Image.gz link do not prove
  secure-firmware behavior or successful Android boot.

After an owner-authorized boot of the paired kernel/vendor build, check that
UNLOCK does not wait for post-fs-data and that the order is CAPI, UNLOCK,
Load TEES, start/end of driver TAs, then ``TEEI BOOT All Completed``. The
``vendor.soter.teei.init`` property should become ``INIT_OK``. Check logcat for
successful keymaster registration and normal encrypted userdata startup.
There must be no ``open km session failed, ffff0008``, open
initialization timeout, ``Too many pending control messages`` or recovery
watchdog. Repeat across cold/warm boots and confirm keystore/RPMB operations;
none of those phone tests was performed while preparing this change.

Read-only boot diagnostics
--------------------------

``/sys/class/teei_config/teei_config/boot_state`` reports started, transport,
final readiness and the cached init result. It exposes no key material and
has no write handler. Read it after boot; an in-progress snapshot need not
be internally atomic. GP-open wait logs identify the blocked process and
stage, while the final TA initialization log includes its result and count.

The cycle-7 expdb slot-2 log identifies ``gfbdce62f70cd``. It still publishes
``All Completed`` at 3.945885 s before loading driver TAs, which only finish
at 4.167592 s: that kernel did not contain this gate. UNLOCK arrives at
3.945607 s and keymaster earlyBootEnded exits successfully at 4.117700 s.
Logging ends around 11.7 s without a panic; this is not evidence that the
TEE gate caused the reset. ``diag_bcb=300`` setting a recovery BCB explains
the next boot destination, not the reset source. Do not clear the BCB or
change that diagnostic policy as a substitute for finding the reset cause.

Cycle-8 preload regression
-------------------------

Fresh expdb slot 1 identifies ``509900a53cc3``. The early-init helper succeeds
at 2.448418 s and UNLOCK arrives at 3.361134 s. CAPI/TEES finish at 3.546 s.
The first requested UUID, ``020b0000000000000000000000000000``, fails at
3.547826 s; the strict gate then caches ``result=-5 ready=0 count=23``.
All subsequent preloads are skipped. This is a gate regression, not failure
of the FBE helper, cmdline delivery, or an observed secure-world panic.

The stock list contains product-optional entries and the original driver
ignored their results. Restore that contract while retaining the final
ordering barrier. Log the full UUID, precise TEEC result/origin on open
failure, and attempted/loaded/missing/failed counts after the complete pass.
The previous ``-EIO`` discarded the secure result: absence of the first TA
file supports an unsupported preload, but does not prove which TEEC error
that boot returned. Do not infer it from ``-5`` or whitelist secure errors.
A failed driver-ID command must also return an error even if its output
parameter is zero; it must not cache a falsely successful loaded session.

The HAL's actual session UUID is ``c09c9c5daa504b78b0e46eda61556c3a``
(its ELF constant passed to TEEC_OpenSession), not preload ``080300...``.
It is present in vendor/thh/ta and opened by the HAL after the preload pass.
Do not add a permanent probe session or require every optional UUID: neither
is part of the vendor ABI. Expected optional preload warnings are not alone
boot failures; successful keymaster registration and encrypted userdata
remain required acceptance checks. No TAs or secure storage are changed.
