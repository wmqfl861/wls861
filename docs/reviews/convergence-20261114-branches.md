# 2026-11-14 收敛分支处置附表

对应[一页收敛报告](../reports/convergence-20261114.md)。批量清理以 `main@71de73d921c0e841be7bc3f4d140e95c12092a8d` 为祖先判断基线；以下是清理前捕获的原末端，已完成清理。

`ancestor` 表示原提交已是该 main 的祖先；`tagged` 表示先推送并核对所列远端标签后删除。表内 SHA 保留完整长度供恢复。同名本地、远端引用分别列出，不能合并计数。`detached:` 两行是脱离分支的工作树标识，不是 Git 引用；仅省略本机绝对路径，其名称、提交和标签完整保留。

## 数字与审核附录差异

| 范围 | 总数 | ancestor | tagged |
| --- | ---: | ---: | ---: |
| 历史远端分支 | 97 | 50 | 47 |
| 历史本地分支 | 139 | 51 | 88 |
| 未被普通引用覆盖的 detached 末端 | 2 | 0 | 2 |
| 合计 | 238 | 101 | 137 |

与[审核附录 A/B](claude-2026-11-14/03-convergence.md#附录-a不是-main-祖先的-46-个分支)的 96 条远端分支相比，唯一新增是 `docs/claude-review-20261114`，原末端 `93bba8a017992b38d280440d105f155bee58ad53`；其余 96 条均存在且末端完全相同。审核目录经 [#96](https://github.com/wmqfl861/wls861/pull/96) 原字节导入，源分支仍按非祖先分支归档。

本地 139 条中，77 条与远端同名，62 条本地独有；下表逐条列出，含保存源码的新分支及原分支上的保存提交。另有 2 个 detached 末端。历史工作树实际移除 146 个，清理 1 个已失效登记。

本轮已合并的 [#95](https://github.com/wmqfl861/wls861/pull/95)、[#96](https://github.com/wmqfl861/wls861/pull/96)、[#97](https://github.com/wmqfl861/wls861/pull/97) 分别删除自身远端分支、本地分支和工作树，共各 3 个，不在上面的历史批量计数内。本报告 PR 的分支和工作树待合并后删除，也不计入已完成数字。

原有 41 个标签全部保留；新增 70 个（先保存 G4 原末端的 2 个，加清点阶段的 68 个），现有 111 个。137 条非祖先记录对应 110 个不同的归档标签，全部已核对远端提交；111 个总标签另含原有发布标签。`converged-20261114` 尚未创建，不计入该数字。

## 原始源码与证据保留

8 个保存提交保留了 19 份原始源码；它们用于归档，没有将历史开发线合入 main：

| 原分支 | 保存前末端 | 保存提交 |
| --- | --- | --- |
| `preserve/convergence-20261114-01` | `a6b8559018a599d7a806b05942857a56d69a00ce` | `01b5c3d9d736a8e72d66b19f725b767d7cb7567a` |
| `preserve/convergence-20261114-02` | `8c3a6955d88170499b129b963164059b03e24312` | `55d4bef26ef4bf11ae16408f4501d87c9e8a4ff3` |
| `fix/immutable-v3-schema-cache-20261113` | `159276417230a8c7e17fdd9723a44782a7b55e9b` | `3ea4063d80d25c15f3d140362155f23dd1dbeb90` |
| `preserve/convergence-20261114-04` | `1e7a54b839da1fa1ec5002bf3372d8ffba2f4934` | `f42e9475e52a17dc1492454bbbda50443adcc0ed` |
| `preserve/convergence-20261114-05` | `fdcc38b4e2de9f28212ba5bd608ba227e7323832` | `7bdd8d41097a081e306609dabf2f7f498429c612` |
| `feat/g1f-fixed-role-tools-20261112-_cyage_5` | `9d9d5dc93e4f99a611a24c14fabbc16cf1f0476b` | `6392c174b694bae5b9140b83fd1123f948e88d7b` |
| `test/g1h-append-fold-20261112` | `83ddb872458534d177141940943b3d3f4e005e98` | `694dd2651e90997cc3754d4e016f77d9128fedff` |
| `diag/n5-persistent-runtime-timeout-20261102-csj777e9` | `79654ca9cca80d5ae8f5204df580797b04963460` | `0c482101c5112bee36e9843bc834ab199fca1b27` |

127 个原始证据文件、6,480,676 bytes 已复制到工作树之外并逐文件核对哈希；至少保留至 2026-12-14。源码归档提交、标签和原失败记录均保留。

保险包 `wls861-all-refs.bundle`：31,030,689 bytes，SHA-256 `ef56e878a7309983f009930855b6c8c1eab74072c1954b0be7cdb2de0f9fee8c`。收敛前全部引用与两处 detached 原提交均已覆盖，bundle verify 通过；保险包、引用清单和相关记录至少保留至 2026-12-14。恢复抽查 `git log -1 archive/detached/g1a-apt-fallback-obi8a5zy` 返回 `b5af9228bc171c8533f4dbb93df918085471b0cf`。

扫描覆盖全部已登记工作树、其中的嵌套 Git 标记和协调者指定的项目目录；范围内未发现独立克隆。权限拒绝的管理员及容器目录、系统私有临时目录、依赖/缓存目录不在完整扫描范围内，目录符号链接未跟随；不宣称扫描了不可读目录。

## 全量处置表

以下 238 行保持捕获顺序；`refs/remotes/origin/` 对应被删除的远端分支，`refs/heads/` 对应本地分支。

| 原引用 / detached 标识 | 原末端 | 处置 | 远端归档标签 |
| --- | --- | --- | --- |
| `refs/heads/ci/g0-events-20261107-iBL7iz` | `162836443d69e2b532867bfbdd61899cd4521a10` | ancestor | `-` |
| `refs/heads/ci/g1-correction-f1-v8-20261108-2140` | `44abefe1925d6a49e9495da99d961647c652aa80` | tagged | `archive/ci/g1-correction-f1-v8-20261108-2140` |
| `refs/heads/ci/g1-correction-red-v8-20261108-2140` | `9932d96d3e6f24ab3a537f74dee943e3b658b49d` | tagged | `archive/ci/g1-correction-red-v8-20261108-2140` |
| `refs/heads/ci/g1a-test-layers-20261111-c9cnzcjo` | `ae0fa40baaef9a64286050a038766d47e26d41d5` | ancestor | `-` |
| `refs/heads/ci/g2-sanitizer-timeouts-20261113` | `e0cc66a7cf471ba264f9bd03c1a0868963240710` | ancestor | `-` |
| `refs/heads/ci/stage0-apt-probe-20261111-2bdaae45` | `b99a6a69aeeb4a5ab5a62351b667e6d8db88a513` | ancestor | `-` |
| `refs/heads/codex/ci-ubsan-ctest-shards` | `57ec0391d8d55ab405a21812e892dcb06c6ea841` | ancestor | `-` |
| `refs/heads/codex/g1-k-response-metadata-20261111` | `457311a72e6bb8c18a3a079f0ba2d0610b0291a5` | ancestor | `-` |
| `refs/heads/codex/g2p-p1a-minimal-contracts` | `c9dcb5c84251c2977d4ec69dc3faca4583216cec` | ancestor | `-` |
| `refs/heads/codex/g2p-p1b-submission-contracts` | `4bca0ba4d47d5a63c1abf1e92bdcdc2f2f6b23e5` | ancestor | `-` |
| `refs/heads/codex/g2p-p2-schema-diagnostics` | `16708117d92332f9ea76976e08631192ab7c2825` | ancestor | `-` |
| `refs/heads/codex/g2p-p5-files-default` | `30250213e607945cb5b52ed2257efe5f4bafbc07` | ancestor | `-` |
| `refs/heads/codex/g2p-p5b-files-default` | `0d4201cd3a295b3bbea6b9ca050000ba91ca8a60` | ancestor | `-` |
| `refs/heads/codex/g2p-p7-rehearsal-30614e` | `274e4bb0989de0c3f788728f8356eab51734aec1` | ancestor | `-` |
| `refs/heads/codex/g4-b-xqrck4ht` | `2ae511a6043291b305c1719246a9d29fe8a99a4b` | tagged | `archive/codex/g4-b-xqrck4ht` |
| `refs/heads/codex/g5-a-adyk5kyp` | `398c935aef9424cee139d448a01479459a1e2a74` | ancestor | `-` |
| `refs/heads/design/n5-1-persistent-events-20261102-csj777e9` | `451b232a0dde08220a5e096d91209087b027b42e` | tagged | `archive/design/n5-1-persistent-events-20261102-csj777e9` |
| `refs/heads/design/n5-2-dag-review-storage-20261102-csj777e9` | `4326ea5004889c4e14b31e26c6b86e6b0a8509b3` | tagged | `archive/design/n5-2-dag-review-storage-20261102-csj777e9` |
| `refs/heads/diag/n5-persistent-runtime-timeout-20261102-csj777e9` | `0c482101c5112bee36e9843bc834ab199fca1b27` | tagged | `archive/diag/n5-persistent-runtime-timeout-20261102-csj777e9` |
| `refs/heads/docs/claude-s1-convergence-20261106-svddnu4h` | `a442b4b95525064e3af949e4c31b15bbb4fb8f72` | tagged | `archive/docs/claude-s1-convergence-20261106-svddnu4h` |
| `refs/heads/docs/f26-handoff-20261111-h6ose7wz` | `ca52d1deb4814e4939f11383c8e31e564820eaef` | ancestor | `-` |
| `refs/heads/docs/facts-g4-g5-u9sl1de_` | `972603150a1049b38b2011dc39776961e9c9bc9a` | ancestor | `-` |
| `refs/heads/docs/g0-closeout-20261107-csuyoyad` | `6afc2888a048b74f37dee19f58dcf7930b615ffc` | ancestor | `-` |
| `refs/heads/docs/g1-main-handoff-20261112-gc8ag9gf` | `4995ab68081f57fd424e02f31948030dd1392038` | ancestor | `-` |
| `refs/heads/docs/g1-started-work-closeout-20261109` | `4987f2c6a6fc0da4b91e53d3689019ba3f0e8631` | ancestor | `-` |
| `refs/heads/docs/g2-review-intake-20261113` | `8433c99813f84557bc310c478665a785250f9c4b` | ancestor | `-` |
| `refs/heads/docs/g2p-exit-20261113-bsjb37ds` | `95ec943f870ace2cf8b204b9a34d2c859d93fc5d` | ancestor | `-` |
| `refs/heads/docs/g3-closeout-vo8z2c45` | `b2b58f9c343a02996e1de4e060501e65ef6cb097` | ancestor | `-` |
| `refs/heads/docs/n4-next-cards-20261102-0k9` | `bdc35045d899c2579bc84ad2ef9f75c2620749f8` | tagged | `archive/docs/n4-next-cards-20261102-0k9` |
| `refs/heads/docs/n5-0-validation-20261102-csj777e9` | `6af0fcb7d95547a4a12d8f9bf0c7e4bba678db3c` | tagged | `archive/docs/n5-0-validation-20261102-csj777e9` |
| `refs/heads/docs/n5-final-validation-20261102-csj777e9` | `f5d1fc77fb886a78bd80ccb901f7dc851ee00b7a` | tagged | `archive/docs/n5-final-validation-20261102-csj777e9` |
| `refs/heads/docs/n5-review-delivery-20261103-74bnxwj5` | `3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123` | tagged | `archive/docs/n5-review-delivery-20261103-74bnxwj5` |
| `refs/heads/docs/n5-review-readiness-20261103-74bnxwj5` | `c25f667e3bc55822612fbacf50b7337a3b79823b` | tagged | `archive/docs/n5-review-readiness-20261103-74bnxwj5` |
| `refs/heads/docs/publication-backfill-20261106-vwxs6crc` | `a90d96fc3e47ecb6397e07fd926bcb7e874f90a5` | tagged | `archive/docs/publication-backfill-20261106-vwxs6crc` |
| `refs/heads/docs/publication-backfill-corrected-20261106-vwxs6crc` | `2d48b3bb4318251c38a00f2a83b516067089a9e0` | tagged | `archive/docs/publication-backfill-corrected-20261106-vwxs6crc` |
| `refs/heads/docs/zcode-diagnostics-report-20261101` | `579e3c7675bc2abb336dde6a95bfddf2139f47cd` | tagged | `archive/docs/zcode-diagnostics-report-20261101` |
| `refs/heads/docs/zcode-final-validation-20261101` | `4289589384d75f29d42233cbaad256ceb711e9bf` | tagged | `archive/docs/zcode-final-validation-20261101` |
| `refs/heads/experimental/controls-partial-20261110-40f7e303` | `f8048ea7da283abe5aa932f375fe3ff671b33a39` | tagged | `archive/experimental/controls-partial-20261110-40f7e303` |
| `refs/heads/feat/blueprint-v0.5.0-and-r2-4-governance` | `079b52766ee0929472165a71a8563ce31671bb46` | tagged | `archive/feat/blueprint-v0.5.0-and-r2-4-governance` |
| `refs/heads/feat/coordinator-async-20261031` | `e78791ca1420204b6205301ec9f22a2e1595fa96` | tagged | `archive/feat/coordinator-async-20261031` |
| `refs/heads/feat/fair-scheduling-20261031` | `86b82f8f63a3c60c7659c9cd8f9b8e7d7252870b` | tagged | `archive/feat/fair-scheduling-20261031` |
| `refs/heads/feat/g1-billing-20261108-y1wf1f4k` | `834efcd91d7b7527300ac05f9086a992a704f00d` | ancestor | `-` |
| `refs/heads/feat/g1-context-20261108-y1wf1f4k` | `0779fe5ba4ce3716db45ed9337f49b0975cd7dda` | tagged | `archive/feat/g1-context-20261108-y1wf1f4k` |
| `refs/heads/feat/g1-context-testclosure-20261109-y1wf1f4k` | `6e5f4e5a0b0ad186c9bf7625b60fbc6aa5caf101` | tagged | `archive/feat/g1-context-testclosure-20261109-y1wf1f4k` |
| `refs/heads/feat/g1-controls-20261108-y1wf1f4k` | `2cf08618f6a2485fa89e2df2bc1182616a2ab231` | tagged | `archive/feat/g1-controls-20261108-y1wf1f4k` |
| `refs/heads/feat/g1-correction-20261108-y1wf1f4k` | `8d26dd3c2f062ac16d9fe02cbb2621bd2f9c37bf` | tagged | `archive/feat/g1-correction-20261108-y1wf1f4k` |
| `refs/heads/feat/g1-integration-i1-20261108-y1wf1f4k` | `cb78849ddf77872f0379331beb7bf5716e4a6a21` | tagged | `archive/feat/g1-integration-i1-20261108-y1wf1f4k` |
| `refs/heads/feat/g1-network-20261108-y1wf1f4k` | `1eb4f21bc673a2d170342dbe3c2b852af846f0c2` | tagged | `archive/feat/g1-network-20261108-y1wf1f4k` |
| `refs/heads/feat/g1-network-resolver-20261109` | `5e1bd127c4ac56576d14d6a45de19a770b1659d1` | tagged | `archive/feat/g1-network-resolver-20261109` |
| `refs/heads/feat/g1-protocol-20261108-y1wf1f4k` | `01c330d697fc2eda928495218a7ba7464dab5a22` | ancestor | `-` |
| `refs/heads/feat/g1b-correction-20261111-qf83ef45` | `c286204b67916be48ec29303b92a4000799dcedf` | ancestor | `-` |
| `refs/heads/feat/g1c-tool-descriptions-20261111-d26da973` | `e13232421c79806ce9d104c3069a9bee37617ca3` | ancestor | `-` |
| `refs/heads/feat/g1d-guidance-20261111-x7ix2whx` | `bc925c36e31f1858e6245282a6afbc34cf3735b5` | ancestor | `-` |
| `refs/heads/feat/g1e-context-layout-20261112-WRrFzVp6` | `2ad515a103fdfaf91256444f3df82863aed9aef3` | ancestor | `-` |
| `refs/heads/feat/g1f-fixed-role-tools-20261112-_cyage_5` | `6392c174b694bae5b9140b83fd1123f948e88d7b` | tagged | `archive/feat/g1f-fixed-role-tools-20261112-_cyage_5` |
| `refs/heads/feat/g1g-lean-v1-1_15ligg` | `22d02a4f60efec6c2b80ec2c95bde832416d04f6` | ancestor | `-` |
| `refs/heads/feat/g1h-append-fold-17k93h8n` | `75e3888d8386e88063a32939ae2aafe78073c352` | ancestor | `-` |
| `refs/heads/feat/g1i-network-20261111-h_n8t220` | `5f36b43c6542186190374337ea5dba4e29077c9f` | ancestor | `-` |
| `refs/heads/feat/g1j-controls-20261111-4zyimpkd` | `ecf66b357147d48a997d66cd201097493114c397` | ancestor | `-` |
| `refs/heads/feat/g2-p3-safe-corrections-20261113` | `0943022a0372d9dee3684e7d68007e36c45e03b8` | ancestor | `-` |
| `refs/heads/feat/g2-p4a-accounting-20261113` | `5842e2ea6e315eed70fb48791717e5da30162349` | ancestor | `-` |
| `refs/heads/feat/g2-p4b-bounded-retries-20261113` | `b148c1d8610773f84f106da453364a8ac8e9d521` | ancestor | `-` |
| `refs/heads/feat/g2-p6-readable-summary-20261113` | `914fa9e0532da25aeb45a4968d07e2f2ae3486c1` | ancestor | `-` |
| `refs/heads/feat/g2-p8-output-metadata-20261113` | `27482ef8841cb0706aac195ab830b8b73786f0cd` | ancestor | `-` |
| `refs/heads/feat/n5-1-async-events-20261102-csj777e9` | `97e3c3f75e35b674d2a254aa0a80bf89ccdf9a59` | tagged | `archive/feat/n5-1-async-events-20261102-csj777e9` |
| `refs/heads/feat/n5-1-persistent-inbox-20261102-csj777e9` | `0c33889ae75e5672f08984cefd90401fa5595e86` | tagged | `archive/feat/n5-1-persistent-inbox-20261102-csj777e9` |
| `refs/heads/feat/n5-3-readonly-identity-20261102-csj777e9` | `af3bd2b14ed497a57453c0261f26d0c6b52190be` | tagged | `archive/feat/n5-3-readonly-identity-20261102-csj777e9` |
| `refs/heads/feat/offline-acceptance-20261031` | `e8a861f32e29e191645f788d20eee8e5ad0e8533` | tagged | `archive/feat/offline-acceptance-20261031` |
| `refs/heads/feat/r2-4-demand-driven-recovery` | `55dc989e02f7e3e2cd1806e53d47298dd8742376` | tagged | `archive/feat/r2-4-demand-driven-recovery` |
| `refs/heads/feat/windows-native-foundation-20261031` | `21f5308271505706594f1a03941ec6ab4a804d50` | tagged | `archive/feat/windows-native-foundation-20261031` |
| `refs/heads/fix/amend-replay-stop-20261031` | `065503e862f3147f3db1ee9e34a129c8091607e4` | tagged | `archive/fix/amend-replay-stop-20261031` |
| `refs/heads/fix/asan-live-profile-20261113-ko_z4bw9` | `07661eb28fd007cd9aee1e45d417493c587921da` | ancestor | `-` |
| `refs/heads/fix/async-review-99c9063-20261031` | `7dc80ed6ec3905e2d20c9d282ef4cb5b692e315a` | tagged | `archive/fix/async-review-99c9063-20261031` |
| `refs/heads/fix/audit-compat-evidence-20261105-adp35vg4` | `7cf8216fd29d72973951b97e312db47bb59624fb` | tagged | `archive/fix/audit-compat-evidence-20261105-adp35vg4` |
| `refs/heads/fix/audit-proc-evidence-20261105-adp35vg4` | `60aafac565b152ee2a8373f74bb860b2ccf24fdb` | tagged | `archive/fix/audit-proc-evidence-20261105-adp35vg4` |
| `refs/heads/fix/audit-product-20261105-adp35vg4` | `2326e521b483bf0a910bc8cac3c96accd38f59f2` | tagged | `archive/fix/audit-product-20261105-adp35vg4` |
| `refs/heads/fix/audit-validation-20261105-adp35vg4` | `a90d96fc3e47ecb6397e07fd926bcb7e874f90a5` | tagged | `archive/fix/audit-validation-20261105-adp35vg4` |
| `refs/heads/fix/audit-windows-evidence-20261105-adp35vg4` | `2719f7103e07a4efb331c1ef2a826329f0ac26fc` | tagged | `archive/fix/audit-windows-evidence-20261105-adp35vg4` |
| `refs/heads/fix/billing-subtest-diagnostics-20261101` | `ceaa9b3e18618c592345001dfc597bdaf579cf81` | tagged | `archive/fix/billing-subtest-diagnostics-20261101` |
| `refs/heads/fix/canonical-validation-no-dom-20261113` | `4ab0468cb284b42e27a5d1675ba618e72d93f25d` | ancestor | `-` |
| `refs/heads/fix/config-canonical-reuse-20261113` | `9a0645d9c85535a50d162428ba07bbf8589aee5b` | ancestor | `-` |
| `refs/heads/fix/cross-pool-resource-fifo-20261031` | `a018d727445def59d7e0f81799793eccc6f23dd4` | tagged | `archive/fix/cross-pool-resource-fifo-20261031` |
| `refs/heads/fix/g0-s1-backport-20261106-wwmxqbtd` | `4d8e28f08c1ce7242fcf3863cd49675f05f1663e` | ancestor | `-` |
| `refs/heads/fix/g1a-make-asan-shards-20261112-jxf2x1g8` | `a862ad102b226792fcbdecfff18667fc74d760fa` | ancestor | `-` |
| `refs/heads/fix/g1hm-reservation-ic14olbu` | `f88fcbb8c27f9b399f52f9b632751d97c55f0959` | tagged | `archive/fix/g1hm-reservation-ic14olbu` |
| `refs/heads/fix/immutable-v3-schema-cache-20261113` | `3ea4063d80d25c15f3d140362155f23dd1dbeb90` | tagged | `archive/fix/immutable-v3-schema-cache-20261113` |
| `refs/heads/fix/n1-sse-guard-allocation-20261102-0k9` | `756946d2a812de3d3a22d800beb244e943c7f47e` | tagged | `archive/fix/n1-sse-guard-allocation-20261102-0k9` |
| `refs/heads/fix/n3-capture-canonical-path-20261102-0k9` | `e23f5d13385b3512d857be34b6003a782965979e` | tagged | `archive/fix/n3-capture-canonical-path-20261102-0k9` |
| `refs/heads/fix/n3-windows-provenance-20261102-0k9` | `c262943670eb32a13e4e6d22b9b68f390f71756d` | tagged | `archive/fix/n3-windows-provenance-20261102-0k9` |
| `refs/heads/fix/n5-0-private-evidence-20261102-csj777e9` | `392040107362d89bccc3b03139ec2340129edbe3` | tagged | `archive/fix/n5-0-private-evidence-20261102-csj777e9` |
| `refs/heads/fix/n5-0-sanitizer-evidence-20261102-csj777e9` | `77cf51265211d9f766926dfb5e6aad59c1604d94` | tagged | `archive/fix/n5-0-sanitizer-evidence-20261102-csj777e9` |
| `refs/heads/fix/n5-1-store411-evaluator-compat-20261102-csj777e9` | `cfd9074f1b420774ddb126757010a0e306cac699` | tagged | `archive/fix/n5-1-store411-evaluator-compat-20261102-csj777e9` |
| `refs/heads/fix/n5-diagnostics-20261104-_7wxnkyn` | `3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123` | tagged | `archive/fix/n5-diagnostics-20261104-_7wxnkyn` |
| `refs/heads/fix/n5-event-validation-local-reuse-20261102-csj777e9` | `2637d415246e2b1b93c3710ea3f3e3f9fb922bc8` | tagged | `archive/fix/n5-event-validation-local-reuse-20261102-csj777e9` |
| `refs/heads/fix/n5-native-capture-fixture-20261102-csj777e9` | `a99ad867a25ad0404bf4b4b7e6a5b76d446022d0` | tagged | `archive/fix/n5-native-capture-fixture-20261102-csj777e9` |
| `refs/heads/fix/n5-native-failure-identities-20261102-csj777e9` | `0c7a7c2478c532fc5114e178df8e63bbc93a8070` | tagged | `archive/fix/n5-native-failure-identities-20261102-csj777e9` |
| `refs/heads/fix/n5-proc-evidence-20261104-_7wxnkyn` | `537b3aef5d5671325239d626448608041de8b79a` | tagged | `archive/fix/n5-proc-evidence-20261104-_7wxnkyn` |
| `refs/heads/fix/n5-review-compatibility-20261103-74bnxwj5` | `3888d95a53572e06825d774aae732d01f7996903` | tagged | `archive/fix/n5-review-compatibility-20261103-74bnxwj5` |
| `refs/heads/fix/n5-review-sanitizer-20261103-74bnxwj5` | `a2ca1055db58dab10aa254029a01c15e1e0c9f18` | tagged | `archive/fix/n5-review-sanitizer-20261103-74bnxwj5` |
| `refs/heads/fix/n5-review-windows-20261103-74bnxwj5` | `8ec950f7ffbf7ece0d9192abc31a60f1c1aa2c29` | tagged | `archive/fix/n5-review-windows-20261103-74bnxwj5` |
| `refs/heads/fix/n5-stop-gate-20261103-74bnxwj5` | `3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123` | tagged | `archive/fix/n5-stop-gate-20261103-74bnxwj5` |
| `refs/heads/fix/n5-windows-evidence-20261104-_7wxnkyn` | `9cd526130609a8eb0187f1ee65cf0b28aa4e182c` | tagged | `archive/fix/n5-windows-evidence-20261104-_7wxnkyn` |
| `refs/heads/fix/native-logs-review-20261031` | `951990071c24fb812569d03d8903e643d52f53b9` | tagged | `archive/fix/native-logs-review-20261031` |
| `refs/heads/fix/sanitizer-followup-20261031` | `1c279f6770f5f4440adcf6b8ee2f51e2401d9a74` | tagged | `archive/fix/sanitizer-followup-20261031` |
| `refs/heads/fix/zcode-local-validation-20261101` | `08dad7e815b9c3c84d3bd2393818b2030a6e0cde` | tagged | `archive/fix/zcode-local-validation-20261101` |
| `refs/heads/g1-l-run-cli-20261112` | `4931cab245f6fcfb95b4afb2ff3f8c9870b894ea` | ancestor | `-` |
| `refs/heads/g1a-context-equivalence-tests-xo6624fx` | `6e0a0cb6f3764e1773dcaa59c67b1b75ce82a1f7` | tagged | `archive/g1a-context-equivalence-tests-xo6624fx` |
| `refs/heads/g3/a1-plain-bzeji0xi` | `7d58b07688c32b8928fa4fb4ac59e9ed9d6f82c6` | ancestor | `-` |
| `refs/heads/g3/a2-evaluation-cyu8euai` | `f6d77520e36f9d69c4de4f3603694acc110886e2` | ancestor | `-` |
| `refs/heads/g4/a1-standard-cgroup-20261113` | `376a4d57b1dbb9e42d3fb923a69aa29ae423d8c5` | tagged | `archive/g4/a1-standard-cgroup-20261113` |
| `refs/heads/g4/c-output-preview-20261113` | `aeb15dce9c75611b32a13a5e081f46ca630a08f0` | ancestor | `-` |
| `refs/heads/g5/b-chat-runtime-__pw741b` | `d7710fabf2a4580a4779f952cf92061469fdd253` | ancestor | `-` |
| `refs/heads/g6/b1-rework-qyh3ond0` | `d27b22402edf0adceb3b78094bacda269f6e9fce` | ancestor | `-` |
| `refs/heads/preserve/convergence-20261114-01` | `01b5c3d9d736a8e72d66b19f725b767d7cb7567a` | tagged | `archive/preserve/convergence-20261114-01` |
| `refs/heads/preserve/convergence-20261114-02` | `55d4bef26ef4bf11ae16408f4501d87c9e8a4ff3` | tagged | `archive/preserve/convergence-20261114-02` |
| `refs/heads/preserve/convergence-20261114-04` | `f42e9475e52a17dc1492454bbbda50443adcc0ed` | tagged | `archive/preserve/convergence-20261114-04` |
| `refs/heads/preserve/convergence-20261114-05` | `7bdd8d41097a081e306609dabf2f7f498429c612` | tagged | `archive/preserve/convergence-20261114-05` |
| `refs/heads/refactor/g0-disposition-helpers-20261107-csuyoyad` | `5073527ffc9052f648cf00785ae5230e1b7c78ad` | ancestor | `-` |
| `refs/heads/refactor/store-current-version-20261107-iBL7iz` | `2159746c849fa1d8a86c0998c1235c94d031aa1f` | ancestor | `-` |
| `refs/heads/test/audit-core-integration-20261105-adp35vg4` | `aa24f1dd02d4a6856aa812b83c64fc7b1407c4a7` | tagged | `archive/test/audit-core-integration-20261105-adp35vg4` |
| `refs/heads/test/g1-i1-native-capture-20261109-y1wf1f4k` | `655f3fcbd832af024b7d7b54347d924e47c59f8f` | tagged | `archive/test/g1-i1-native-capture-20261109-y1wf1f4k` |
| `refs/heads/test/g1b-cli-expectations-20261111-ggngfsxo` | `d0e98f3f71090ea8d77e71bc377b4485733d9ae2` | tagged | `archive/test/g1b-cli-expectations-20261111-ggngfsxo` |
| `refs/heads/test/g1h-append-fold-20261112` | `694dd2651e90997cc3754d4e016f77d9128fedff` | tagged | `archive/test/g1h-append-fold-20261112` |
| `refs/heads/test/g1m-exit-g1m-exit-fH2QBQGx` | `c2aa6440dec010b9161527c1c80d6b75ccf2081a` | ancestor | `-` |
| `refs/heads/test/n2-permanent-evidence-20261102-0k9` | `6f20bf8aff74114d060dfa38f2fee6f76e5c0298` | tagged | `archive/test/n2-permanent-evidence-20261102-0k9` |
| `refs/heads/test/n5-compatibility-20261104-_7wxnkyn` | `efcd4547bd7627fa821a4b7bce097e633b514982` | tagged | `archive/test/n5-compatibility-20261104-_7wxnkyn` |
| `refs/heads/test/n5-crash-stage-20261104-_7wxnkyn` | `3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123` | tagged | `archive/test/n5-crash-stage-20261104-_7wxnkyn` |
| `refs/heads/test/n5-persistent-workflow-20261104-_7wxnkyn` | `a5675e1245bebba41c499e56bd2c8a16be2d5dae` | tagged | `archive/test/n5-persistent-workflow-20261104-_7wxnkyn` |
| `refs/heads/test/n5-review-persistent-20261103-74bnxwj5` | `3a2dd55d51e32e7e49ed546ccb4c441d3bd96782` | tagged | `archive/test/n5-review-persistent-20261103-74bnxwj5` |
| `refs/heads/test/r2-4-recovery-boundary-coverage` | `1a5ca4f3549e1531066a159b0e410e69b1a57573` | tagged | `archive/test/r2-4-recovery-boundary-coverage` |
| `refs/heads/verify/g0-merge-pr46-f7760e5` | `bd0a58b5eb6371eaf9f9c9776fb55f518432aadc` | tagged | `archive/verify/g0-merge-pr46-f7760e5` |
| `refs/heads/verify/g4-ab-hyulgojo` | `cccc057002a02e197a2138fcc9ca08c8b7796f02` | tagged | `archive/verify/g4-ab-hyulgojo` |
| `refs/heads/work/billing-main-integration-20261111-h6ose7wz` | `ed243d49484dbf73b526c1e9c3ac65dfc6caf512` | ancestor | `-` |
| `refs/heads/work/coordinator-n0-n4-20261102-0k9` | `aa2c592b8aed6509189710cc6f6d69e0959098a8` | tagged | `archive/work/coordinator-n0-n4-20261102-0k9` |
| `refs/heads/work/n5-final-candidate-20261102-csj777e9` | `c2a5cea64aeea261b214cdfd9a968b53f491110a` | tagged | `archive/work/n5-final-candidate-20261102-csj777e9` |
| `refs/heads/work/n5-integrated-candidate-20261102-csj777e9` | `79654ca9cca80d5ae8f5204df580797b04963460` | tagged | `archive/work/n5-integrated-candidate-20261102-csj777e9` |
| `refs/heads/work/n5-review-integration-20261103-74bnxwj5` | `f15bf1a944a1ea5f6b9e5e9696b921caa48f04cc` | tagged | `archive/work/n5-review-integration-20261103-74bnxwj5` |
| `refs/heads/work/next-cycle-20261031-065247` | `772feaee62086ddb0e7f605a7472721d3bc61406` | tagged | `archive/work/next-cycle-20261031-065247` |
| `refs/heads/work/next-cycle-integration-20261031` | `99c9063f3e29cd6d69c56fb15815715d499a374a` | tagged | `archive/work/next-cycle-integration-20261031` |
| `refs/remotes/origin/ci/g0-events-20261107-iBL7iz` | `162836443d69e2b532867bfbdd61899cd4521a10` | ancestor | `-` |
| `refs/remotes/origin/ci/g1-correction-f1-v8-20261108-2140` | `44abefe1925d6a49e9495da99d961647c652aa80` | tagged | `archive/ci/g1-correction-f1-v8-20261108-2140` |
| `refs/remotes/origin/ci/g1-correction-red-v8-20261108-2140` | `9932d96d3e6f24ab3a537f74dee943e3b658b49d` | tagged | `archive/ci/g1-correction-red-v8-20261108-2140` |
| `refs/remotes/origin/ci/g1a-test-layers-20261111-c9cnzcjo` | `ae0fa40baaef9a64286050a038766d47e26d41d5` | ancestor | `-` |
| `refs/remotes/origin/ci/g2-sanitizer-timeouts-20261113` | `e0cc66a7cf471ba264f9bd03c1a0868963240710` | ancestor | `-` |
| `refs/remotes/origin/ci/stage0-apt-probe-20261111-2bdaae45` | `b99a6a69aeeb4a5ab5a62351b667e6d8db88a513` | ancestor | `-` |
| `refs/remotes/origin/codex/ci-ubsan-ctest-shards` | `57ec0391d8d55ab405a21812e892dcb06c6ea841` | ancestor | `-` |
| `refs/remotes/origin/codex/g1-k-response-metadata-20261111` | `457311a72e6bb8c18a3a079f0ba2d0610b0291a5` | ancestor | `-` |
| `refs/remotes/origin/codex/g2p-p1a-minimal-contracts` | `c9dcb5c84251c2977d4ec69dc3faca4583216cec` | ancestor | `-` |
| `refs/remotes/origin/codex/g2p-p1b-submission-contracts` | `4bca0ba4d47d5a63c1abf1e92bdcdc2f2f6b23e5` | ancestor | `-` |
| `refs/remotes/origin/codex/g2p-p2-schema-diagnostics` | `16708117d92332f9ea76976e08631192ab7c2825` | ancestor | `-` |
| `refs/remotes/origin/codex/g2p-p5-files-default` | `30250213e607945cb5b52ed2257efe5f4bafbc07` | ancestor | `-` |
| `refs/remotes/origin/codex/g2p-p5b-files-default` | `0d4201cd3a295b3bbea6b9ca050000ba91ca8a60` | ancestor | `-` |
| `refs/remotes/origin/codex/g2p-p7-rehearsal-30614e` | `274e4bb0989de0c3f788728f8356eab51734aec1` | ancestor | `-` |
| `refs/remotes/origin/codex/g4-b-xqrck4ht` | `2ae511a6043291b305c1719246a9d29fe8a99a4b` | tagged | `archive/codex/g4-b-xqrck4ht` |
| `refs/remotes/origin/codex/g5-a-adyk5kyp` | `398c935aef9424cee139d448a01479459a1e2a74` | ancestor | `-` |
| `refs/remotes/origin/docs/claude-audit-20261111` | `7d1dadb17bd5d089c23d1dbb086c694991c1e6e2` | tagged | `archive/docs/claude-audit-20261111` |
| `refs/remotes/origin/docs/claude-review-20261113` | `45055214639969df5512171758d19db653d71614` | tagged | `archive/docs/claude-review-20261113` |
| `refs/remotes/origin/docs/claude-review-20261114` | `93bba8a017992b38d280440d105f155bee58ad53` | tagged | `archive/docs/claude-review-20261114` |
| `refs/remotes/origin/docs/claude-s1-convergence-20261106-svddnu4h` | `a442b4b95525064e3af949e4c31b15bbb4fb8f72` | tagged | `archive/docs/claude-s1-convergence-20261106-svddnu4h` |
| `refs/remotes/origin/docs/coordinator-handoff-4289589-20261101` | `c33978ca62fe3fc63b4cf3440afdc4f5b647f13b` | tagged | `archive/docs/coordinator-handoff-4289589-20261101` |
| `refs/remotes/origin/docs/coordinator-handoff-aa2-20261102` | `aa2c592b8aed6509189710cc6f6d69e0959098a8` | tagged | `archive/docs/coordinator-handoff-aa2-20261102` |
| `refs/remotes/origin/docs/coordinator-handoff-aa2c592-20261102` | `283d23070a1f36f2eea598bcb96b2a312cd71307` | tagged | `archive/docs/coordinator-handoff-aa2c592-20261102` |
| `refs/remotes/origin/docs/coordinator-review-n5-20261102` | `d843493fafe158f3a6443bafaa0de95cbf6c05c2` | tagged | `archive/docs/coordinator-review-n5-20261102` |
| `refs/remotes/origin/docs/f26-handoff-20261111-h6ose7wz` | `ca52d1deb4814e4939f11383c8e31e564820eaef` | ancestor | `-` |
| `refs/remotes/origin/docs/facts-g4-g5-u9sl1de_` | `972603150a1049b38b2011dc39776961e9c9bc9a` | ancestor | `-` |
| `refs/remotes/origin/docs/g0-closeout-20261107-csuyoyad` | `6afc2888a048b74f37dee19f58dcf7930b615ffc` | ancestor | `-` |
| `refs/remotes/origin/docs/g1-main-handoff-20261112-gc8ag9gf` | `4995ab68081f57fd424e02f31948030dd1392038` | ancestor | `-` |
| `refs/remotes/origin/docs/g2-review-intake-20261113` | `8433c99813f84557bc310c478665a785250f9c4b` | ancestor | `-` |
| `refs/remotes/origin/docs/g2p-exit-20261113-bsjb37ds` | `95ec943f870ace2cf8b204b9a34d2c859d93fc5d` | ancestor | `-` |
| `refs/remotes/origin/docs/g3-closeout-vo8z2c45` | `b2b58f9c343a02996e1de4e060501e65ef6cb097` | ancestor | `-` |
| `refs/remotes/origin/docs/n4-next-cards-20261102-0k9` | `bdc35045d899c2579bc84ad2ef9f75c2620749f8` | tagged | `archive/docs/n4-next-cards-20261102-0k9` |
| `refs/remotes/origin/docs/n5-review-readiness-20261103-74bnxwj5` | `c25f667e3bc55822612fbacf50b7337a3b79823b` | tagged | `archive/docs/n5-review-readiness-20261103-74bnxwj5` |
| `refs/remotes/origin/docs/publication-backfill-corrected-20261106-vwxs6crc` | `2d48b3bb4318251c38a00f2a83b516067089a9e0` | tagged | `archive/docs/publication-backfill-corrected-20261106-vwxs6crc` |
| `refs/remotes/origin/docs/review-attachments-99c9063f` | `ce518883d890d4bef62457aeed2e1b4ff1ec6ec4` | tagged | `archive/docs/review-attachments-99c9063f` |
| `refs/remotes/origin/experimental/controls-partial-20261110-40f7e303` | `f8048ea7da283abe5aa932f375fe3ff671b33a39` | tagged | `archive/experimental/controls-partial-20261110-40f7e303` |
| `refs/remotes/origin/feat/g1-billing-20261108-y1wf1f4k` | `ed243d49484dbf73b526c1e9c3ac65dfc6caf512` | ancestor | `-` |
| `refs/remotes/origin/feat/g1-context-20261108-y1wf1f4k` | `0779fe5ba4ce3716db45ed9337f49b0975cd7dda` | tagged | `archive/feat/g1-context-20261108-y1wf1f4k` |
| `refs/remotes/origin/feat/g1-context-testclosure-20261109-y1wf1f4k` | `6e5f4e5a0b0ad186c9bf7625b60fbc6aa5caf101` | tagged | `archive/feat/g1-context-testclosure-20261109-y1wf1f4k` |
| `refs/remotes/origin/feat/g1-controls-20261108-y1wf1f4k` | `2cf08618f6a2485fa89e2df2bc1182616a2ab231` | tagged | `archive/feat/g1-controls-20261108-y1wf1f4k` |
| `refs/remotes/origin/feat/g1-correction-20261108-y1wf1f4k` | `8d26dd3c2f062ac16d9fe02cbb2621bd2f9c37bf` | tagged | `archive/feat/g1-correction-20261108-y1wf1f4k` |
| `refs/remotes/origin/feat/g1-integration-i1-20261108-y1wf1f4k` | `cb78849ddf77872f0379331beb7bf5716e4a6a21` | tagged | `archive/feat/g1-integration-i1-20261108-y1wf1f4k` |
| `refs/remotes/origin/feat/g1-network-20261108-y1wf1f4k` | `1eb4f21bc673a2d170342dbe3c2b852af846f0c2` | tagged | `archive/feat/g1-network-20261108-y1wf1f4k` |
| `refs/remotes/origin/feat/g1-network-resolver-20261109` | `5e1bd127c4ac56576d14d6a45de19a770b1659d1` | tagged | `archive/feat/g1-network-resolver-20261109` |
| `refs/remotes/origin/feat/g1-protocol-20261108-y1wf1f4k` | `01c330d697fc2eda928495218a7ba7464dab5a22` | ancestor | `-` |
| `refs/remotes/origin/feat/g1b-correction-20261111-qf83ef45` | `c286204b67916be48ec29303b92a4000799dcedf` | ancestor | `-` |
| `refs/remotes/origin/feat/g1c-tool-descriptions-20261111-d26da973` | `e13232421c79806ce9d104c3069a9bee37617ca3` | ancestor | `-` |
| `refs/remotes/origin/feat/g1d-guidance-20261111-x7ix2whx` | `bc925c36e31f1858e6245282a6afbc34cf3735b5` | ancestor | `-` |
| `refs/remotes/origin/feat/g1e-context-layout-20261112-WRrFzVp6` | `2ad515a103fdfaf91256444f3df82863aed9aef3` | ancestor | `-` |
| `refs/remotes/origin/feat/g1f-fixed-role-tools-20261112-_cyage_5` | `9d9d5dc93e4f99a611a24c14fabbc16cf1f0476b` | ancestor | `-` |
| `refs/remotes/origin/feat/g1g-lean-v1-1_15ligg` | `22d02a4f60efec6c2b80ec2c95bde832416d04f6` | ancestor | `-` |
| `refs/remotes/origin/feat/g1h-append-fold-17k93h8n` | `75e3888d8386e88063a32939ae2aafe78073c352` | ancestor | `-` |
| `refs/remotes/origin/feat/g1i-network-20261111-h_n8t220` | `5f36b43c6542186190374337ea5dba4e29077c9f` | ancestor | `-` |
| `refs/remotes/origin/feat/g1j-controls-20261111-4zyimpkd` | `ecf66b357147d48a997d66cd201097493114c397` | ancestor | `-` |
| `refs/remotes/origin/feat/g2-p3-safe-corrections-20261113` | `0943022a0372d9dee3684e7d68007e36c45e03b8` | ancestor | `-` |
| `refs/remotes/origin/feat/g2-p4a-accounting-20261113` | `5842e2ea6e315eed70fb48791717e5da30162349` | ancestor | `-` |
| `refs/remotes/origin/feat/g2-p4b-bounded-retries-20261113` | `b148c1d8610773f84f106da453364a8ac8e9d521` | ancestor | `-` |
| `refs/remotes/origin/feat/g2-p6-readable-summary-20261113` | `914fa9e0532da25aeb45a4968d07e2f2ae3486c1` | ancestor | `-` |
| `refs/remotes/origin/feat/g2-p8-output-metadata-20261113` | `27482ef8841cb0706aac195ab830b8b73786f0cd` | ancestor | `-` |
| `refs/remotes/origin/fix/amend-replay-stop-20261031` | `065503e862f3147f3db1ee9e34a129c8091607e4` | tagged | `archive/fix/amend-replay-stop-20261031` |
| `refs/remotes/origin/fix/asan-live-profile-20261113-ko_z4bw9` | `07661eb28fd007cd9aee1e45d417493c587921da` | ancestor | `-` |
| `refs/remotes/origin/fix/canonical-validation-no-dom-20261113` | `4ab0468cb284b42e27a5d1675ba618e72d93f25d` | ancestor | `-` |
| `refs/remotes/origin/fix/config-canonical-reuse-20261113` | `9a0645d9c85535a50d162428ba07bbf8589aee5b` | ancestor | `-` |
| `refs/remotes/origin/fix/cross-pool-resource-fifo-20261031` | `a018d727445def59d7e0f81799793eccc6f23dd4` | tagged | `archive/fix/cross-pool-resource-fifo-20261031` |
| `refs/remotes/origin/fix/g0-s1-backport-20261106-wwmxqbtd` | `4d8e28f08c1ce7242fcf3863cd49675f05f1663e` | ancestor | `-` |
| `refs/remotes/origin/fix/g1a-make-asan-shards-20261112-jxf2x1g8` | `a862ad102b226792fcbdecfff18667fc74d760fa` | ancestor | `-` |
| `refs/remotes/origin/fix/g1hm-reservation-ic14olbu` | `f88fcbb8c27f9b399f52f9b632751d97c55f0959` | tagged | `archive/fix/g1hm-reservation-ic14olbu` |
| `refs/remotes/origin/fix/n1-sse-guard-allocation-20261102-0k9` | `756946d2a812de3d3a22d800beb244e943c7f47e` | tagged | `archive/fix/n1-sse-guard-allocation-20261102-0k9` |
| `refs/remotes/origin/fix/n3-capture-canonical-path-20261102-0k9` | `e23f5d13385b3512d857be34b6003a782965979e` | tagged | `archive/fix/n3-capture-canonical-path-20261102-0k9` |
| `refs/remotes/origin/fix/n5-0-private-evidence-20261102-csj777e9` | `392040107362d89bccc3b03139ec2340129edbe3` | tagged | `archive/fix/n5-0-private-evidence-20261102-csj777e9` |
| `refs/remotes/origin/fix/native-logs-review-20261031` | `951990071c24fb812569d03d8903e643d52f53b9` | tagged | `archive/fix/native-logs-review-20261031` |
| `refs/remotes/origin/g1-l-run-cli-20261112` | `4931cab245f6fcfb95b4afb2ff3f8c9870b894ea` | ancestor | `-` |
| `refs/remotes/origin/g3/a1-plain-bzeji0xi` | `7d58b07688c32b8928fa4fb4ac59e9ed9d6f82c6` | ancestor | `-` |
| `refs/remotes/origin/g3/a2-evaluation-cyu8euai` | `f6d77520e36f9d69c4de4f3603694acc110886e2` | ancestor | `-` |
| `refs/remotes/origin/g4/a1-standard-cgroup-20261113` | `376a4d57b1dbb9e42d3fb923a69aa29ae423d8c5` | tagged | `archive/g4/a1-standard-cgroup-20261113` |
| `refs/remotes/origin/g4/c-output-preview-20261113` | `aeb15dce9c75611b32a13a5e081f46ca630a08f0` | ancestor | `-` |
| `refs/remotes/origin/g5/b-chat-runtime-__pw741b` | `d7710fabf2a4580a4779f952cf92061469fdd253` | ancestor | `-` |
| `refs/remotes/origin/g6/b1-rework-qyh3ond0` | `d27b22402edf0adceb3b78094bacda269f6e9fce` | ancestor | `-` |
| `refs/remotes/origin/refactor/g0-disposition-helpers-20261107-csuyoyad` | `5073527ffc9052f648cf00785ae5230e1b7c78ad` | ancestor | `-` |
| `refs/remotes/origin/refactor/store-current-version-20261107-iBL7iz` | `2159746c849fa1d8a86c0998c1235c94d031aa1f` | ancestor | `-` |
| `refs/remotes/origin/test/audit-core-integration-20261105-adp35vg4` | `aa24f1dd02d4a6856aa812b83c64fc7b1407c4a7` | tagged | `archive/test/audit-core-integration-20261105-adp35vg4` |
| `refs/remotes/origin/test/g1-i1-native-capture-20261109-y1wf1f4k` | `655f3fcbd832af024b7d7b54347d924e47c59f8f` | tagged | `archive/test/g1-i1-native-capture-20261109-y1wf1f4k` |
| `refs/remotes/origin/test/g1m-exit-g1m-exit-fH2QBQGx` | `c2aa6440dec010b9161527c1c80d6b75ccf2081a` | ancestor | `-` |
| `refs/remotes/origin/test/n2-permanent-evidence-20261102-0k9` | `6f20bf8aff74114d060dfa38f2fee6f76e5c0298` | tagged | `archive/test/n2-permanent-evidence-20261102-0k9` |
| `refs/remotes/origin/verify/g0-merge-pr44-d28c4a54-csuyoyad` | `f75e245102fe3078505704d3bba25979b06880e7` | tagged | `archive/verify/g0-merge-pr44-d28c4a54-csuyoyad` |
| `refs/remotes/origin/verify/g0-merge-pr45-f75e2451-csuyoyad` | `f2381e5acfd420fa0f6175023adb6996a81503af` | tagged | `archive/verify/g0-merge-pr45-f75e2451-csuyoyad` |
| `refs/remotes/origin/verify/g0-merge-pr46-f7760e5` | `bd0a58b5eb6371eaf9f9c9776fb55f518432aadc` | tagged | `archive/verify/g0-merge-pr46-f7760e5` |
| `refs/remotes/origin/work/audit24-publish-673a6df` | `8ccd11a80cf70f0623881f6a65fcc8535e83b10e` | tagged | `archive/work/audit24-publish-673a6df` |
| `refs/remotes/origin/work/claude-audit-import` | `ede6fc036c71091448736b14bfa2f2fdc23a0eeb` | tagged | `archive/work/claude-audit-import` |
| `refs/remotes/origin/work/r1-history-publish-cb6a52a` | `6f49ef673f36295c6e3bd8641b1906363154c042` | tagged | `archive/work/r1-history-publish-cb6a52a` |
| `refs/remotes/origin/work/r12-publish-f89cb04` | `dda0638f6a407415e44becfba3bab702dce03eb7` | tagged | `archive/work/r12-publish-f89cb04` |
| `refs/remotes/origin/work/r13-publish-6066eb7` | `29589e9ccc324a009e203f753e3028a0ec5dbd37` | tagged | `archive/work/r13-publish-6066eb7` |
| `refs/remotes/origin/work/r2-rejection-followup` | `5a9629bcfcbfbb92435f5f02e30cba3801aacd48` | tagged | `archive/work/r2-rejection-followup` |
| `refs/remotes/origin/work/r21-publish-fdc39d8` | `d0f240dd264efb737e4f6c1b1c0385cde927ac80` | tagged | `archive/work/r21-publish-fdc39d8` |
| `refs/remotes/origin/work/r21-publish-scope` | `06a80f2f3cb81f59613737af91022078ec7e1ee8` | tagged | `archive/work/r21-publish-scope` |
| `refs/remotes/origin/work/r22-transport-c975be3` | `27bee81d304d8cd2f2120236f54f55013ca10199` | tagged | `archive/work/r22-transport-c975be3` |
| `refs/remotes/origin/work/s1.6-owned-cleanup` | `445bceaf9f62a1ee6e6aed46b3dfc313243e6658` | tagged | `archive/work/s1.6-owned-cleanup` |
| `detached:g1a-apt-fallback-obi8a5zy` | `b5af9228bc171c8533f4dbb93df918085471b0cf` | tagged | `archive/detached/g1a-apt-fallback-obi8a5zy` |
| `detached:g1a-json-tests-t0ejb6yo` | `f98f31bcf0f6ccac76ccc78eae6bafe3d12317bb` | tagged | `archive/detached/g1a-json-tests-t0ejb6yo` |
