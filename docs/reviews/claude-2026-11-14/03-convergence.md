# 03 最后收敛：只保留 main

依据 `02-decisions.md` 的 D-19、D-20、D-22、D-23、D-24、D-29。由本地 agent 执行。文中“11-13 的某文件”指 `claude-2026-11-13/` 目录下的同名文件。

## 1. 目标与完成的标准

做完以后，下面七条同时成立：

1. 远端只有一个分支 `main`（`git ls-remote --heads origin` 只有一行）。
2. 没有打开的 PR。
3. 本机只有一个工作树，位于 `main`；只有一个本地分支 `main`，与 `origin/main` 相同；没有这个仓库的其他克隆目录。
4. `main` 最新提交的完整矩阵 9 个 job 全部成功，`acceptance_runtime` 各注册在 CMake ASan 下不超过 90 秒。
5. 每一个被删除、又不是 `main` 祖先的分支，都有一个远端可见的 `archive/` 标签指向它原来的末端。
6. 仓库之外有一个包含收敛前全部引用的保险包。
7. `main` 里有一页收敛报告，`main` 的最终提交上有标签 `converged-20261114`。

## 2. 不做的事

- 不强推，不改写历史，不删 `main`，不删任何标签，不删仓库里的任何文件或目录。
- 不把历史分支合进 `main`。要合并的只有三样：CI-1 的修复、本目录的审核文档、G4 的两条在途分支（D-22）。
- 不拿本文附录当删除清单。删除清单以执行时自己算出来的为准，附录只用来对照。

## 3. 步骤

下面的命令是示意，路径和细节按本机实际调整。`$KEEP` 指仓库之外、任何工作树之外的一个归档目录。第 0、4、5、6、7 步的命令和 §4 的三种恢复办法，在审核环境里对这个仓库的一份本地副本完整演练过一遍【实测】：96 个分支里 50 个直接删、46 个打标签后删，最后只剩 `main`；从标签、从提交号、从保险包恢复各试了一次，都成功。演练没有碰真实的远端。

### 第 0 步　准备和保险

```sh
KEEP=<仓库之外的归档目录>/20261114 && mkdir -p "$KEEP"
git fetch --all --prune --tags
git fetch origin '+refs/pull/*/head:refs/remotes/pull/*'      # 把已关闭 PR 的末端也取下来
git for-each-ref --format='%(refname) %(objectname)' > "$KEEP/refs-before.txt"
git ls-remote origin > "$KEEP/remote-before.txt"
git worktree list --porcelain > "$KEEP/worktrees-before.txt"
git bundle create "$KEEP/wls861-all-refs.bundle" --all
git bundle verify "$KEEP/wls861-all-refs.bundle"
```

- 本机如果还有这个仓库的其他克隆目录，对每个克隆都做一遍：有没推送的提交，先推成一个分支，让它进入第 4 步的清点。
- 给 `main` 加保护（D-24 第 4 项）。令牌没有权限就跳过，在报告里写一行：

```sh
gh api -X PUT repos/wmqfl861/wls861/branches/main/protection --input - <<'EOF'
{"required_status_checks": null, "enforce_admins": true,
 "required_pull_request_reviews": null, "restrictions": null,
 "allow_force_pushes": false, "allow_deletions": false}
EOF
```

- 保险包和三份清单至少保留到 2026-12-14。

### 第 1 步　让 main 稳定变绿（CI-1、CI-2）

一个 PR，只改测试和 CI，不改产品代码。内容见 `02-decisions.md` §3.1。

1. `acceptance_runtime_tests` 加 `--group`，CTest 拆成 3 到 4 个注册。
2. `.github/workflows/ci.yml` 的“只改文档”清单加上 `CHANGELOG.md`、`AGENTS.md`。
3. 合并之前在分支上手动触发一次完整矩阵：`gh workflow run ci.yml --ref <分支> -f verification=full`。9 个 job 全绿，并且每个新注册在 ASan-1/2、ASan-2/2 日志里的耗时不超过 90 秒。
4. 合并。等 `main` 上的完整矩阵跑完并通过。它是后面所有步骤的前提。

### 第 2 步　合入本轮审核文档

开一个文档 PR，把分支 `docs/claude-review-20261114` 的 `docs/reviews/claude-2026-11-14/` 合入 `main`。可以直接合并这个分支，也可以像前两轮那样导入内容。

### 第 3 步　处置 G4 的两条在途分支（D-22）

1. `g4/a1-standard-cgroup-20261113`（PR #84）变基到最新 `main`，解除草稿状态。
2. `codex/g4-b-xqrck4ht` 变基到 G4-A1 之上。它在 `src/store.cpp` 里加的 `validate_process_runtime_root` 要按 #93 之后的文件布局落位。开 PR。
3. 每个 PR：自己的三项核心检查全绿，并且在分支上手动跑一次完整矩阵全绿，然后合并。先 A1 后 B。
4. 可选，只试一次：在手动的 `standard-native` 作业里加 `sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0`，跑一次原生检查，结果照实记录。
5. 走退路的条件和做法见 D-22 第 5 点。走退路时，这两条分支留给第 5 步统一打标签，这里只需要关闭 #84 并写评论。

这一步结束时，没有打开的 PR。

### 第 4 步　清点

1. 再执行一次 `git fetch --all --prune --tags`。
2. 对每个本机工作树执行 `git -C <路径> status --porcelain`。有没提交的改动：是在途工作，就提交到它所在的分支；是构建产物或临时文件，不用管。`git stash list` 不为空时同样处理：是在途工作就放到一个分支上提交，否则丢弃。
3. 算出每个分支（远端的和本机的）是不是 `origin/main` 的祖先，写进处置表，并给不是祖先的打标签：

```sh
main=$(git rev-parse origin/main)
: > "$KEEP/disposition.tsv"
git for-each-ref --format='%(refname) %(objectname)' refs/remotes/origin refs/heads |
while read -r ref sha; do
  case "$ref" in refs/remotes/origin/HEAD|refs/remotes/origin/main|refs/heads/main) continue ;; esac
  name=${ref#refs/remotes/origin/}; name=${name#refs/heads/}
  if git merge-base --is-ancestor "$sha" "$main"; then
    printf '%s\t%s\tancestor\t-\n' "$ref" "$sha" >> "$KEEP/disposition.tsv"
  else
    tag="archive/$name"
    have=$(git rev-parse -q --verify "refs/tags/$tag^{commit}" || true)
    if [ -n "$have" ] && [ "$have" != "$sha" ]; then
      tag="$tag-$(printf %.7s "$sha")"
      have=$(git rev-parse -q --verify "refs/tags/$tag^{commit}" || true)
    fi
    [ "$have" = "$sha" ] || git tag "$tag" "$sha" || exit 1
    printf '%s\t%s\ttagged\t%s\n' "$ref" "$sha" "$tag" >> "$KEEP/disposition.tsv"
  fi
done
```

4. 把处置表和本文附录对照。附录是 2026-11-14 上午 `main@b7c10dd` 时的状态：96 个远端分支，50 个是祖先，46 个不是。不一致的地方（新增的分支、末端变了的分支、本机独有的分支）在收敛报告里逐条列出。前三步新开又合并掉的分支会出现在“祖先”一类，属于正常；本轮的审核分支 `docs/claude-review-20261114` 是附录之后才推送的，也不在附录里。

### 第 5 步　推送标签并核对

```sh
git push origin 'refs/tags/archive/*'
git ls-remote --tags origin 'refs/tags/archive/*' > "$KEEP/remote-tags-after.txt"
awk -F'\t' '$3=="tagged"{print $2" refs/tags/"$4}' "$KEEP/disposition.tsv" |
while read -r sha ref; do
  grep -q "^$sha[[:space:]]$ref\$" "$KEEP/remote-tags-after.txt" || echo "MISSING $ref"
done
```

没有任何 `MISSING` 才能进入下一步。有的话先补推、查明原因。

### 第 6 步　删除远端分支

```sh
git ls-remote --heads origin | awk '{print $2}' | sed 's#^refs/heads/##' | grep -vx main > "$KEEP/remote-heads-to-delete.txt"
```

删除之前核对两件事：清单里没有 `main`；清单里每个名字都在处置表里，并且状态是 `ancestor`，或者是 `tagged` 且第 5 步核对通过。然后：

```sh
xargs -n 20 git push origin --delete < "$KEEP/remote-heads-to-delete.txt"
git ls-remote --heads origin          # 期望只有一行 refs/heads/main
```

### 第 7 步　清理本机

1. `main` 以外的每个工作树：里面如果有不足 30 天的原始证据，先整体移到 `$KEEP` 旁边的归档目录（D-12）；构建产物不用留。然后 `git worktree remove --force <路径>`。
2. 收尾：

```sh
git worktree prune
git switch main && git merge --ff-only origin/main
git for-each-ref --format='%(refname:short)' refs/heads | grep -vx main | xargs -r -n 20 git branch -D
git for-each-ref --format='delete %(refname)' refs/remotes/pull | git update-ref --stdin
git remote prune origin
```

3. 删除这个仓库的其他克隆目录（第 0 步已确认它们没有独有的提交）。

### 第 8 步　核对、报告、标记

1. 核对 §1 的七条。另外抽一个新打的标签验证能恢复：`git log -1 archive/<某个分支名>` 能显示处置表里记的那个提交。
2. 打开“合并后自动删除分支”（D-24 第 1 项）：`gh api -X PATCH repos/wmqfl861/wls861 -F delete_branch_on_merge=true`。没有权限就写进报告。
3. 写一页报告 `docs/reports/convergence-20261114.md`，同一个 PR 里更新 STATUS（开头三行、当前 CI、G4 一行，正文压到 60 行以内）和 11-13 `04-plan.md` 的状态列。报告的内容：

| 项目 | 说明 |
| --- | --- |
| 结果 | §1 的七条逐条写“是”或“否”；“否”的写原因 |
| 数字 | 删除的远端分支数（祖先多少、打标签多少）；删除的本机分支数和工作树数；新增的标签数 |
| CI | 第 1 步 PR 的完整矩阵链接；`main` 最终提交的完整矩阵链接；`acceptance_runtime` 各注册在 ASan 下的耗时 |
| G4 | 合并了（PR 链接）还是停放了（标签名、原因）；托管 runner 上原生检查的结果 |
| 与审核清单的差异 | 第 4 步对照出来的不一致，逐条列出 |
| 保险包 | 文件名、大小、保留到哪天。不写本机的绝对路径 |
| 附表 | 处置表全文：分支名、原末端、处置（祖先 / 打标签）、标签名 |

4. 报告的 PR 合并后删除它自己的分支。然后在最终的 `main` 提交上打标签：

```sh
git fetch origin && git switch main && git merge --ff-only origin/main
git tag -a converged-20261114 -m "Only main remains. See docs/reports/convergence-20261114.md" main
git push origin converged-20261114
```

## 4. 出了岔子怎么办

| 情况 | 做法 |
| --- | --- |
| 第 1 步拆完之后 ASan 下仍有注册超过 90 秒 | 继续拆。不提高时限 |
| 第 1 步合并后 `main` 的完整矩阵因为别的原因失败 | 按 D-21 先修。修好之前不进入第 3 步以后的步骤 |
| G4 变基冲突大，或完整矩阵两轮不过 | 走 D-22 的退路，不要为了合并去改设计 |
| 推送标签失败，或核对出现 `MISSING` | 停在第 5 步，不删任何分支，先查明原因 |
| 删除远端分支时部分失败 | 重新执行第 6 步的清单生成和删除，已删的不会再出现在清单里 |
| 误删了需要的分支 | 是祖先的：`git push origin <提交号>:refs/heads/<分支名>`，提交号在处置表里。打过标签的：`git push origin archive/<分支名>^{commit}:refs/heads/<分支名>`。两者都失效时用保险包：`git fetch "$KEEP/wls861-all-refs.bundle" '<原引用>:refs/heads/<分支名>'` |
| 发现本机有处置表里没有的未推送提交 | 停下来，把它推成分支，从第 4 步重新做 |

## 5. 收敛之后保持的状态

见 D-24：合并即删分支和工作树；远端只有 `main` 加不超过 3 个在制 PR 的分支；要停放的工作用 `archive/` 标签，不留分支。

## 附录 A：不是 main 祖先的 46 个分支

2026-11-14 上午、`main@b7c10dd` 时的状态。“领先 main”是该分支有而 main 没有的提交数。“说明”只用于识别，是按分支名、分叉点和改动的文件归类的；处置规则不依赖它。

处置：两条“在途”分支按 D-22；其余 44 条一律打 `archive/<分支名>` 标签后删除。

| 分支 | 末端 | 末次提交 | 领先 main | 类别 | 说明 |
| --- | --- | --- | ---: | --- | --- |
| `work/claude-audit-import` | `ede6fc0` | 10-22 | 1 | 历史 | 10 月的发布辅助分支，只含 .development/ 与工作流文件 |
| `work/s1.6-owned-cleanup` | `445bcea` | 10-22 | 4 | 历史 | 10 月旧开发分支，44/45 个文件已在 main，余下 1 个是旧工作流文件 |
| `work/audit24-publish-673a6df` | `8ccd11a` | 10-24 | 1 | 历史 | 10 月的发布辅助分支，只含 .development/ 与工作流文件 |
| `work/r1-history-publish-cb6a52a` | `6f49ef6` | 10-24 | 1 | 历史 | 10 月的发布辅助分支，只含 .development/ 与工作流文件 |
| `work/r12-publish-f89cb04` | `dda0638` | 10-24 | 1 | 历史 | 10 月的发布辅助分支，只含 .development/ 与工作流文件 |
| `work/r13-publish-6066eb7` | `29589e9` | 10-25 | 1 | 历史 | 10 月的发布辅助分支，只含 .development/ 与工作流文件 |
| `work/r2-rejection-followup` | `5a9629b` | 10-25 | 2 | 历史 | 10 月旧开发分支，未合并 |
| `work/r21-publish-fdc39d8` | `d0f240d` | 10-25 | 1 | 历史 | 10 月的发布辅助分支，只含 .development/ 与工作流文件 |
| `work/r21-publish-scope` | `06a80f2` | 10-25 | 1 | 历史 | 10 月的发布辅助分支，只含 .development/ 与工作流文件 |
| `work/r22-transport-c975be3` | `27bee81` | 10-25 | 1 | 历史 | 10 月的发布辅助分支，只含 .development/ 与工作流文件 |
| `docs/review-attachments-99c9063f` | `ce51888` | 10-31 | 33 | 历史 | 协调器 / N 系列开发线（10-31 至 11-06），11-06 收敛时未采纳 |
| `fix/amend-replay-stop-20261031` | `065503e` | 10-31 | 33 | 历史 | 协调器 / N 系列开发线（10-31 至 11-06），11-06 收敛时未采纳 |
| `fix/cross-pool-resource-fifo-20261031` | `a018d72` | 10-31 | 33 | 历史 | 协调器 / N 系列开发线（10-31 至 11-06），11-06 收敛时未采纳 |
| `fix/native-logs-review-20261031` | `9519900` | 10-31 | 34 | 历史 | 协调器 / N 系列开发线（10-31 至 11-06），11-06 收敛时未采纳 |
| `docs/coordinator-handoff-4289589-20261101` | `c33978c` | 11-01 | 42 | 历史 | 协调器 / N 系列开发线（10-31 至 11-06），11-06 收敛时未采纳 |
| `docs/coordinator-handoff-aa2-20261102` | `aa2c592` | 11-02 | 61 | 历史 | 协调器 / N 系列开发线（10-31 至 11-06），11-06 收敛时未采纳 |
| `docs/coordinator-handoff-aa2c592-20261102` | `283d230` | 11-02 | 62 | 历史 | 协调器 / N 系列开发线（10-31 至 11-06），11-06 收敛时未采纳 |
| `docs/n4-next-cards-20261102-0k9` | `bdc3504` | 11-02 | 45 | 历史 | 协调器 / N 系列开发线（10-31 至 11-06），11-06 收敛时未采纳 |
| `fix/n1-sse-guard-allocation-20261102-0k9` | `756946d` | 11-02 | 45 | 历史 | 协调器 / N 系列开发线（10-31 至 11-06），11-06 收敛时未采纳 |
| `fix/n3-capture-canonical-path-20261102-0k9` | `e23f5d1` | 11-02 | 45 | 历史 | 协调器 / N 系列开发线（10-31 至 11-06），11-06 收敛时未采纳 |
| `fix/n5-0-private-evidence-20261102-csj777e9` | `3920401` | 11-02 | 62 | 历史 | 协调器 / N 系列开发线（10-31 至 11-06），11-06 收敛时未采纳 |
| `test/n2-permanent-evidence-20261102-0k9` | `6f20bf8` | 11-02 | 51 | 历史 | 协调器 / N 系列开发线（10-31 至 11-06），11-06 收敛时未采纳 |
| `docs/coordinator-review-n5-20261102` | `d843493` | 11-03 | 118 | 历史 | 协调器 / N 系列开发线（10-31 至 11-06），11-06 收敛时未采纳 |
| `docs/n5-review-readiness-20261103-74bnxwj5` | `c25f667` | 11-03 | 115 | 历史 | 协调器 / N 系列开发线（10-31 至 11-06），11-06 收敛时未采纳 |
| `test/audit-core-integration-20261105-adp35vg4` | `aa24f1d` | 11-05 | 129 | 历史 | 协调器 / N 系列开发线（10-31 至 11-06），11-06 收敛时未采纳 |
| `docs/publication-backfill-corrected-20261106-vwxs6crc` | `2d48b3b` | 11-06 | 130 | 历史 | 协调器 / N 系列开发线（10-31 至 11-06），11-06 收敛时未采纳；PR #42（已关闭） |
| `docs/claude-s1-convergence-20261106-svddnu4h` | `a442b4b` | 11-07 | 1 | 历史 | 11-06 收敛文档的早期稿，main 里是后来的版本 |
| `verify/g0-merge-pr44-d28c4a54-csuyoyad` | `f75e245` | 11-07 | 1 | 内容已在 main | 验证或修复分支，文件内容与 main 一致或已在 main 的历史里 |
| `verify/g0-merge-pr45-f75e2451-csuyoyad` | `f2381e5` | 11-07 | 2 | 内容已在 main | 验证或修复分支，文件内容与 main 一致或已在 main 的历史里 |
| `ci/g1-correction-f1-v8-20261108-2140` | `44abefe` | 11-08 | 1 | 历史 | G1 阶段的旧分支，对应功能已由 #53–#66 分小块合入 |
| `ci/g1-correction-red-v8-20261108-2140` | `9932d96` | 11-08 | 1 | 历史 | G1 阶段的旧分支，对应功能已由 #53–#66 分小块合入 |
| `verify/g0-merge-pr46-f7760e5` | `bd0a58b` | 11-08 | 1 | 内容已在 main | 验证或修复分支，文件内容与 main 一致或已在 main 的历史里 |
| `feat/g1-context-20261108-y1wf1f4k` | `0779fe5` | 11-09 | 2 | 历史 | G1 阶段的旧分支，对应功能已由 #53–#66 分小块合入 |
| `feat/g1-integration-i1-20261108-y1wf1f4k` | `cb78849` | 11-09 | 3 | 历史 | G1 阶段的旧分支，对应功能已由 #53–#66 分小块合入 |
| `feat/g1-network-20261108-y1wf1f4k` | `1eb4f21` | 11-09 | 1 | 历史 | G1 阶段的旧分支，对应功能已由 #53–#66 分小块合入 |
| `test/g1-i1-native-capture-20261109-y1wf1f4k` | `655f3fc` | 11-09 | 4 | 历史 | G1 阶段的旧分支，对应功能已由 #53–#66 分小块合入 |
| `feat/g1-controls-20261108-y1wf1f4k` | `2cf0861` | 11-10 | 2 | 历史 | G1 阶段的旧分支，对应功能已由 #53–#66 分小块合入 |
| `docs/claude-audit-20261111` | `7d1dadb` | 11-11 | 1 | 内容已在 main | 审核文档分支，内容已由 #51、#68 导入 |
| `experimental/controls-partial-20261110-40f7e303` | `f8048ea` | 11-11 | 3 | 历史 | G1 阶段的旧分支，对应功能已由 #53–#66 分小块合入 |
| `feat/g1-context-testclosure-20261109-y1wf1f4k` | `6e5f4e5` | 11-11 | 10 | 历史 | G1 阶段的旧分支，对应功能已由 #53–#66 分小块合入 |
| `feat/g1-correction-20261108-y1wf1f4k` | `8d26dd3` | 11-11 | 2 | 历史 | G1 阶段的旧分支，对应功能已由 #53–#66 分小块合入 |
| `feat/g1-network-resolver-20261109` | `5e1bd12` | 11-11 | 4 | 历史 | G1 阶段的旧分支，对应功能已由 #53–#66 分小块合入；PR #50（已关闭） |
| `fix/g1hm-reservation-ic14olbu` | `f88fcbb` | 11-12 | 2 | 内容已在 main | 验证或修复分支，文件内容与 main 一致或已在 main 的历史里 |
| `codex/g4-b-xqrck4ht` | `2ae511a` | 11-13 | 5 | 在途 | G4-B 候选，没有 PR，依赖 G4-A1 |
| `docs/claude-review-20261113` | `4505521` | 11-13 | 1 | 内容已在 main | 审核文档分支，内容已由 #51、#68 导入 |
| `g4/a1-standard-cgroup-20261113` | `376a4d5` | 11-13 | 1 | 在途 | G4-A1，PR #84（草稿） |

这 46 个里，只有 `docs/coordinator-handoff-aa2-20261102` 和 `docs/n5-review-readiness-20261103-74bnxwj5` 两个的末端已经被现有的 `archive/` 标签保留；其余 44 个在打标签之前没有任何标签指向。

## 附录 B：已经是 main 祖先的 50 个分支

全部对应已合并的 PR，直接删除，不需要标签。

| PR | 分支 | 末端 |
| --- | --- | --- |
| #43 | `fix/g0-s1-backport-20261106-wwmxqbtd` | `4d8e28f` |
| #44 | `ci/g0-events-20261107-iBL7iz` | `1628364` |
| #45 | `refactor/store-current-version-20261107-iBL7iz` | `2159746` |
| #46 | `refactor/g0-disposition-helpers-20261107-csuyoyad` | `5073527` |
| #47 | `docs/g0-closeout-20261107-csuyoyad` | `6afc288` |
| #48 | `feat/g1-protocol-20261108-y1wf1f4k` | `01c330d` |
| #49 | `feat/g1-billing-20261108-y1wf1f4k` | `ed243d4` |
| #51 | `docs/f26-handoff-20261111-h6ose7wz` | `ca52d1d` |
| #52 | `ci/stage0-apt-probe-20261111-2bdaae45` | `b99a6a6` |
| #53 | `ci/g1a-test-layers-20261111-c9cnzcjo` | `ae0fa40` |
| #54 | `feat/g1i-network-20261111-h_n8t220` | `5f36b43` |
| #55 | `feat/g1b-correction-20261111-qf83ef45` | `c286204` |
| #56 | `feat/g1c-tool-descriptions-20261111-d26da973` | `e132324` |
| #57 | `feat/g1d-guidance-20261111-x7ix2whx` | `bc925c3` |
| #58 | `feat/g1j-controls-20261111-4zyimpkd` | `ecf66b3` |
| #59 | `codex/g1-k-response-metadata-20261111` | `457311a` |
| #60 | `feat/g1e-context-layout-20261112-WRrFzVp6` | `2ad515a` |
| #61 | `g1-l-run-cli-20261112` | `4931cab` |
| #62 | `fix/g1a-make-asan-shards-20261112-jxf2x1g8` | `a862ad1` |
| #63 | `feat/g1f-fixed-role-tools-20261112-_cyage_5` | `9d9d5dc` |
| #64 | `feat/g1g-lean-v1-1_15ligg` | `22d02a4` |
| #65 | `feat/g1h-append-fold-17k93h8n` | `75e3888` |
| #66 | `test/g1m-exit-g1m-exit-fH2QBQGx` | `c2aa644` |
| #67 | `docs/g1-main-handoff-20261112-gc8ag9gf` | `4995ab6` |
| #68 | `docs/g2-review-intake-20261113` | `8433c99` |
| #69 | `codex/g2p-p2-schema-diagnostics` | `1670811` |
| #70 | `feat/g2-p4a-accounting-20261113` | `5842e2e` |
| #71 | `ci/g2-sanitizer-timeouts-20261113` | `e0cc66a` |
| #72 | `feat/g2-p6-readable-summary-20261113` | `914fa9e` |
| #73 | `codex/g2p-p1a-minimal-contracts` | `c9dcb5c` |
| #74 | `feat/g2-p8-output-metadata-20261113` | `27482ef` |
| #75 | `feat/g2-p4b-bounded-retries-20261113` | `b148c1d` |
| #76 | `codex/ci-ubsan-ctest-shards` | `57ec039` |
| #77 | `codex/g2p-p5-files-default` | `3025021` |
| #78 | `feat/g2-p3-safe-corrections-20261113` | `0943022` |
| #79 | `codex/g2p-p1b-submission-contracts` | `4bca0ba` |
| #80 | `codex/g2p-p5b-files-default` | `0d4201c` |
| #81 | `codex/g2p-p7-rehearsal-30614e` | `274e4bb` |
| #82 | `docs/g2p-exit-20261113-bsjb37ds` | `95ec943` |
| #83 | `fix/canonical-validation-no-dom-20261113` | `4ab0468` |
| #85 | `fix/config-canonical-reuse-20261113` | `9a0645d` |
| #86 | `g4/c-output-preview-20261113` | `aeb15dc` |
| #87 | `codex/g5-a-adyk5kyp` | `398c935` |
| #88 | `g5/b-chat-runtime-__pw741b` | `d7710fa` |
| #89 | `docs/facts-g4-g5-u9sl1de_` | `9726031` |
| #90 | `fix/asan-live-profile-20261113-ko_z4bw9` | `07661eb` |
| #91 | `g3/a1-plain-bzeji0xi` | `7d58b07` |
| #92 | `g3/a2-evaluation-cyu8euai` | `f6d7752` |
| #93 | `g6/b1-rework-qyh3ond0` | `d27b224` |
| #94 | `docs/g3-closeout-vo8z2c45` | `b2b58f9` |
