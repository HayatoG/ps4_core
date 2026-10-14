# Hooks e perfil do jogo / Game hooks and profile

[English below](#english) · Branch `feat/game-hooks` (14/10/2026) · Itens 3.1–3.3 do `~/decomp/PROXIMOS_PASSOS.md`.

## 1. Tabela de hooks (`port/src/runtime_game.h`)

O núcleo não cita nenhum jogo. Tudo o que é do jogo entra por uma tabela que o port define em `port/src/game_<slug>.c`:

| Campo | Para quê |
|---|---|
| `slug`, `name` | identidade (log) |
| `title_ids` | lista terminada em `NULL`; o loader avisa se o `param.sfo` montado não estiver nela (e roda mesmo assim) |
| `env_prefix` | `"ISS_"`: cada `ISS_X` vira `TLG_X` no início. `-DPORT_ENV_PREFIX` (do `port.env`) tem prioridade |
| `profile` | nome do perfil, `"profile.ini"` |
| `resolvers[]` | as funções `runtime_<slug>*_resolve` do jogo, **em ordem**, terminada em `NULL`; tentadas depois dos módulos do núcleo e antes da GPU |
| `init` | chamado logo depois do `runtime_start()` |
| `patch_image` | patches de código no eboot (depois das relocações e dos patches de dados) |
| `report` | linhas extras no relatório final |

**Como o build escolhe a tabela** (`build.sh`; `PORT_GAME` no CMake do Windows):
1. `PORT_GAME=<slug>` no ambiente;
2. senão `PORT_SLUG` do `port/packaging/port.env`, se existir `src/game_<PORT_SLUG>.c`;
3. senão o único `src/game_*.c` além do `game_none.c` (mais de um = erro);
4. senão `src/game_none.c` (tudo `NULL`: build só do núcleo, testes, CI).

O log do build diz `Game hooks: src/game_<slug>.c`.

**Jogo novo** (ex.: The Last of Us Remastered): só `src/game_<slug>.c` (+ `src/runtime_<slug>.c` se precisar de funções próprias, declarando o resolver no próprio `game_<slug>.c`), `packaging/port.env` (com `PORT_SLUG`) e `profile.ini`. Nada no núcleo muda.

**Os três jogos atuais** (modelos prontos em `port/games/`, mesma ordem de resolução de antes):

| Port | `resolvers[]` | `report` | `env_prefix` |
|---|---|---|---|
| `tlg_pc` (`game_tlg.c`) | `runtime_tlg` | — | — (`TLG_` é o do núcleo) |
| `gr2_pc` (`game_gr2.c`) | `gr2_audio3d`, `tlg`, `gr2`, `gr2_np`, `gr2_font`, `gr2_json` | `runtime_gr2_audio3d_report` | — (`GR2_` planejado) |
| `infamous_pc` (`game_infamous.c`) | os mesmos + `infamous` | `runtime_gr2_audio3d_report` | `"ISS_"` (dispensa o `PORT_ENV_PREFIX` no `port.env`) |

**Conferido sem rodar jogo:** um programa de teste resolveu todos os imports de cada `boot-linked.bin` (TLG 642, GR2 981, inFAMOUS 564 nomes, função e dado) com o núcleo antigo (`09cd813`) + os arquivos do port e com este núcleo + `game_<slug>.c` + os arquivos aparados. Mesma função para todos os nomes nos três jogos; só mudou o arquivo das funções que subiram (`runtime_eventflag.c`, `runtime_system.c`).

## 2. Funções que subiram para o núcleo

| Novo arquivo | Funções | De |
|---|---|---|
| `runtime_eventflag.c` | `sceKernel{Create,Delete,Set,Clear,Poll,Wait,Cancel}EventFlag` | `runtime_tlg.c` |
| `runtime_system.c` | `sceKernelIsNeoMode`, `AvailableFlexibleMemorySize`, `Getdents`, `GetPrtAperture`, `GetModuleInfoForUnwind`, `InternalMemoryGetModuleSegmentInfo`, `ConvertUtcToLocaltime`/`LocaltimeToUtc`, `GetCompiledSdkVersion`, `sceLibcHeapGetTraceInfo`, `pthread_equal`, `sincosf`, `sceSystemServiceLoadExec`, `sceAudioOutSetMixLevelPadSpk`, `sceDiscMap_7C980FFB0AA27E7A`, `scePlayGoDialog*` | `runtime_tlg.c` |
| `runtime_system.c` | `sceErrorDialog*`, `scePlayGoDialogClose` | `runtime_gr2.c` |

Código igual ao dos ports. Ficam no jogo: os placeholders do NGS2 (e os de Audio3d no `runtime_tlg.c` do TLG). O `scePadSetMotionSensorState` do `runtime_tlg.c` já era sombreado pelo `runtime_pad.c` e sai também.

**Migração de cada port** (num PR do port, com teste de boot): copiar o núcleo, copiar `port/games/game_<slug>.c` para `port/src/`, aplicar `port/games/migration/<port>.patch` (tira as cópias do `runtime_tlg.c`/`runtime_gr2.c`; confere com `git apply --check` na `main` de hoje), copiar `port/games/profile_<slug>.ini` para `port/profile.ini`. As cópias que ficarem não quebram nada (os módulos do núcleo vêm antes), mas são código morto.

## 3. Perfil do jogo (`port/profile.ini`, `port/src/runtime_profile.c`)

**Precedência:** padrões do núcleo < `profile.ini` < launcher/jogador < ambiente. O carregador roda no começo do `main`, depois do alias do prefixo, e só preenche as `TLG_*` que ninguém definiu (o launcher passa as escolhas do jogador por ambiente, então elas ganham). O log diz `Profile: readbacks=2 -> TLG_READBACKS` ou `... ignored, TLG_X=... set by the launcher or the environment`.

**Onde:** `TLG_PROFILE=<arquivo>` (ou `ISS_PROFILE` pelo alias; `0`/`none` desliga); senão `runtime_game.profile` no diretório de trabalho (o `run.sh` roda em `port/`), ao lado do executável, ou um nível acima (pacotes: o `package.sh` copia o `profile.ini` para a raiz).

**Formato:** `chave = valor`; `#` ou `;` no começo da linha ou depois de espaço abre comentário; seções `[...]`.

| Seção / chave | Variável | Valores |
|---|---|---|
| `tsc` | `TLG_TSC` | `host` (padrão do núcleo), `process`, `auto` |
| `tsc_auto_threshold` | `TLG_TSC_AUTO_THRESHOLD` | sítios para `auto` escolher `host` (64) |
| `readbacks` | `TLG_READBACKS` | 0 desligado, 1 relaxado (padrão), 2 preciso |
| `eager_readback`, `write_merge` | `TLG_EAGER_READBACK`, `TLG_WRITE_MERGE` | 0/1 (só com readback preciso) |
| `gc_budget_mb` | `TLG_GC_BUDGET_MB` | MB do cache de texturas (0 = o do driver) |
| `block1d` | `TLG_BLOCK1D` | 0/1 |
| `skip_adjacency` | `TLG_SKIP_ADJACENCY` | 0/1 (0 deixa a variável sem definir: o núcleo só olha se ela existe) |
| `pipeline_binaries` | `TLG_PIPELINE_BINARIES` | 0/1 |
| `mouse_buttons` | `TLG_MOUSE_BUTTONS` | `Mouse Left=square,Mouse Right=l2,Mouse Middle=none` (`left=` também vale) |
| `motion_button`, `stick_deadzone` | `TLG_MOTION_BUTTON`, `TLG_STICK_DEADZONE` | como no README |
| `[env]` `NOME = valor` | `TLG_NOME` | qualquer outra variável do núcleo (só as lidas do ambiente; para chaves do `tlg.ini` use `[settings]`) |
| `[settings]` `chave = valor` | — | padrões de chaves do `tlg.ini` (`mouse_sensitivity`, `smaa`…), lidos antes do `tlg.ini` do jogador (`tlg_settings.cpp`, via `TLG_PROFILE_LOADED`) |

**Botões do mouse:** o mapa embutido do `runtime_pad.c` é o do TLG (esquerdo Triângulo, direito R1, meio Quadrado). `mouse_buttons` troca só as fontes listadas; o `key_map` do jogador (`tlg.ini` ou `TLG_KEY_MAP`) é aplicado depois e ganha.

**`tsc=auto`:** depois de ler a imagem, o `probe.c` conta `0F 31` nos segmentos executáveis do eboot (fora dos módulos linkados: libc, Fios2…). Um `0F 31` seguido de byte ModRM registrador-registrador (`C0`–`FF`, exceto `C4`/`C5`/`E8`/`E9`/`EB`) é quase sempre `0F` + `xor r32,r32`, não `rdtsc`, e não conta. Medido contra o `objdump`: TLG 662 (642 de verdade, 769 brutos), GR2 465 (453, 830), inFAMOUS 403 (391, 505). Com 64 ou mais → `TLG_TSC=host`, abaixo → `process`. Log: `TSC: auto: 403 rdtsc sites in the eboot code (505 raw 0F 31 in 12.8 MB), threshold 64 -> host rdtsc (measured frequency)`. Os três jogos atuais ficam em `host`, como hoje.

Modelos: `port/games/profile_{tlg,gr2,infamous}.ini`, com os padrões que hoje estão nos launchers (`DEFAULT_ENV`/`DEFAULT_SETTINGS`).

---

<a id="english"></a>

## English

**Hook table** (`port/src/runtime_game.h`): the core names no game; the port defines `const RuntimeGame runtime_game` in `port/src/game_<slug>.c` (slug, name, title IDs, env prefix, profile, ordered `resolvers[]`, `init`, `patch_image`, `report`). The build picks `PORT_GAME`, else `PORT_SLUG` from `packaging/port.env` when `src/game_<slug>.c` exists, else the only `src/game_*.c`, else `src/game_none.c` (core-only). **A new game needs only `src/game_<slug>.c`** (+ optional `src/runtime_<slug>.c`), `port.env` and `profile.ini`. Ready tables for TLG, GR2 and inFAMOUS are in `port/games/`, with the same resolver order as the old fixed calls (verified offline: every import of each game's `boot-linked.bin` resolves to the same function before and after).

**Moved into the core:** event flags (`runtime_eventflag.c`); kernel queries, time zone conversion, unwind/module info, PRT aperture, heap trace, SDK version, `pthread_equal`, `sincosf`, LoadExec, pad speaker mix, DiscMap, PlayGo dialog (from `runtime_tlg.c`) and ErrorDialog + `scePlayGoDialogClose` (from `runtime_gr2.c`) in `runtime_system.c`. Ports drop their copies with `port/games/migration/<port>.patch`.

**Profile** (`port/profile.ini`, `runtime_profile.c`): core defaults < profile < launcher/player < environment; fills only unset `TLG_*`. Lookup: `TLG_PROFILE` (`0`/`none` disables), else `runtime_game.profile` in the working directory, next to the executable, or one level up. Keys: `tsc` (host|process|auto), `tsc_auto_threshold`, `readbacks`, `eager_readback`, `write_merge`, `gc_budget_mb`, `block1d`, `skip_adjacency`, `pipeline_binaries`, `mouse_buttons`, `motion_button`, `stick_deadzone`; `[env]` for any other env-only `TLG_X`; `[settings]` for `tlg.ini` defaults (read before the player's `tlg.ini`). `tsc=auto` counts `0F 31` in the prepared eboot's executable segments at load (ignoring hits followed by a register-register ModRM, which are `0F` + `xor r32,r32`), picks host at ≥ 64 sites and logs the decision; TLG/GR2/inFAMOUS all pick host, as today.
