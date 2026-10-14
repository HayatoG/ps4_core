<h1 align="center">ps4_core — núcleo compartilhado dos ports nativos de PS4</h1>

<p align="center">
  <a href="#english"><b>English below</b></a> ·
  <a href="docs/LIBRARIES.md">Bibliotecas</a> ·
  <a href="docs/KNOWN_ISSUES.md">Problemas conhecidos</a> ·
  <a href="docs/CHANGELOG.md">Changelog</a> ·
  <a href="https://www.youtube.com/@hayatog-dev">YouTube</a> ·
  <a href="https://discord.gg/UMkdMYZh9g">Discord</a>
</p>

> **Repositório só local.** Sem GitHub, sem remoto, sem push (por enquanto). É aqui que se acumula tudo o que um jogo de PS4 precisa para rodar nativo no PC, e que hoje vai copiado em cada port: [tlg_pc](https://github.com/HayatoG/tlg_pc) (The Last Guardian), [gr2_pc](https://github.com/HayatoG/gr2_pc) (Gravity Rush 2) e `infamous_pc` (inFAMOUS Second Son, GitHub `iss_pc`).

Por **HayatoG** · Licença GPL-2.0-or-later (`port/LICENSE`, `port/UPSTREAM.md`) · **Nenhum arquivo da Sony entra aqui** (eboot, prx, sprx, psarc, sfo, arte oficial): o `.gitignore` barra os formatos.

## O que é

Um port nativo roda o executável do jogo (o `eboot.bin` descriptografado do dump do jogador) direto no x86-64 do PC, sem emular a CPU. Para isso o núcleo fornece três coisas:

1. **Loader e linker** (`port/src/probe.c`, `port/scripts/link_modules.py`, `link_libc.py`): mapeiam o eboot e os módulos que vêm com o jogo (`libc.prx`, `libSceFios2.prx`…), aplicam relocações, ligam cada import `NID#biblioteca` a uma função do host e corrigem o que o PC faz diferente (por exemplo, `fs:[0]` → `gs`).
2. **As bibliotecas do sistema do PS4 reimplementadas (HLE)** em C (`port/src/runtime_*.c`): libkernel (threads, sincronização, memória, arquivos, tempo), Pad, AudioOut, Ajm (ATRAC9/MP3), SaveData, UserService, SystemService, Rtc, AppContent, Sysmodule, PlayGo, Videodec, NpManager/Trophy offline, Http/Ssl/Net stubs, diálogos… O catálogo completo, função por função, com o que é real e o que é stub e quais jogos usam: **[docs/LIBRARIES.md](docs/LIBRARIES.md)**.
3. **A camada de GPU** (`port/gpu/`): um fork do shadPS4 que traduz os command buffers do GCN (libSceGnmDriver) e os shaders para Vulkan, mais VideoOut, AvPlayer e o `shim/` (janela SDL3, overlay, menu Insert, upscalers, pós-processamento).

Cada port fica só com o que é do seu jogo: os `runtime_<slug>*.c` com as funções que só ele usa, patches, perfil, launcher, ícone e documentação.

## Layout

```
ps4_core/
  README.md, CORE_MANIFEST, CORE_SOURCE
  docs/      LIBRARIES.md (gerado), KNOWN_ISSUES.md, CHANGELOG.md
  tools/     core_sync.py (sincronizar com os ports), list_exports.py (gera docs/LIBRARIES.md)
  scripts/   ferramentas genéricas: make_game_view.py, session_chart.py, sce_imports.py, psarc_toc.py, FTP/USB
  port/      mesmos caminhos de um repositório de port:
    src/       probe.c (loader), runtime.c (resolve), runtime_*.c (bibliotecas), import_names.inc,
               runtime_game.h (rascunho da tabela de hooks), mouse_stick.h, compat_win.*, *_hook.h
    scripts/   prepare.py, link_modules.py, link_libc.py, content_profile.py, patches.py, mods.py, …
    gpu/       shadps4/ (fork), shim/, CMakeLists.txt, patches/, dlss_bridge/, third_party/ (submódulos)
    third_party/LibAtrac9 (submódulo)
    tests/, tools/, docs/ (GPU), packaging/ (genérico), launcher/tlg_vulkan.py
    build.sh, run.sh, run.py, msys2-env.sh, shell.nix
```

O **`CORE_MANIFEST`** é a lista exata do que é núcleo (globs, exclusões `!` e submódulos `@`) e a fonte de verdade do `core_sync.py`. O **`CORE_SOURCE`** diz de qual commit de cada port veio cada parte. Os caminhos são **iguais** aos de um port, então comparar e copiar é direto. A raiz `docs/` e `tools/` são só deste repositório (não entram no manifesto).

### Submódulos

`fsr-vulkan`, `imgui`, `magic_enum`, `miniz`, `vulkan-headers`, `xbyak` (em `port/gpu/third_party/`) e `port/third_party/LibAtrac9`, nos mesmos commits da `main` do inFAMOUS, com o `.gitmodules` nas URLs oficiais. Num clone novo: `git submodule update --init` (com rede) ou, offline, apontar `submodule.<nome>.url` para um checkout local e usar `git -c protocol.file.allow=always submodule update --init`.

## O que é núcleo e o que é de cada jogo

| Núcleo (`ps4_core`) | De cada jogo (fica no port) |
|---|---|
| `port/src/` menos o que está à direita | `port/src/runtime_<slug>*.c`: `runtime_tlg.c`, `runtime_gr2*.c`, `runtime_infamous.c` |
| `import_names.inc` (**união** de todos os jogos) | `port/src/game_<slug>.c` (futuro): a tabela `RuntimeGame` |
| scripts de preparação e link, `patches.py` (o mecanismo) | `port/scripts/<slug>_patches.py` (ex.: FPS do TLG) |
| GPU inteira (shadPS4 + shim), submódulos | `port/profile.ini` (futuro): padrões do jogo |
| empacotamento genérico | `port/packaging/port.env`, `icon/`, `README-player.md`, `windows/*.rc` |
| `tlg_vulkan.py` | `port/launcher/<slug>_*`, `<slug>_launcher.sh`, `run_<slug>.sh` |
| ferramentas genéricas de `scripts/` | `scripts/<slug>_run.sh` e roteiros de teste; README, `docs/`, CI, `dump/`, `game/` |

**`import_names.inc` é uma união.** A tabela NID → nome não é só diagnóstico: o `runtime_lookup` resolve imports pelo nome, então um nome faltando é uma função que não liga, e uma entrada a mais não custa nada. Um jogo novo acrescenta as suas linhas aqui (gerar com `scripts/sce_imports.py`). O `docs/LIBRARIES.md` lista os nomes das tabelas que ainda não têm linha.

## Como um port consome o núcleo

**Hoje:** cada port tem a sua cópia dos arquivos do manifesto, mantida em paralelo com o `tools/core_sync.py` (abaixo).

**Plano:**
1. **Submódulo em `port/core`** (caminho local até existir um remoto). O `build.sh` e o `CMakeLists.txt` do port ficam finos: incluem `core/port/gpu` e compilam `core/port/src/*.c` + os `src/runtime_<slug>*.c` + `src/game_<slug>.c` do próprio port.
2. **Hooks do jogo por tabela**, não por nome fixo. Hoje o `runtime.c` chama `runtime_tlg_resolve`, `runtime_gr2*_resolve` e `runtime_infamous_resolve` diretamente (por isso o gr2_pc ainda carrega o `runtime_tlg.c`, e o infamous_pc carrega todos). O rascunho está em **`port/src/runtime_game.h`** (comentado, ainda não ligado): `slug`, `name`, `title_ids`, `env_prefix`, `profile`, `resolvers[]` (em ordem, terminada em NULL), `init`, `patch_image` (patches de código, como os cheats do GoW3 que hoje estão no `probe.c`) e `report`. O port define `const RuntimeGame runtime_game` em `src/game_<slug>.c`; um `game_none.c` vazio permite build e testes só do núcleo.
3. **O que dois jogos usam sobe para o núcleo.** A seção "Cobertura por jogo" do `docs/LIBRARIES.md` mostra quais imports cada jogo resolve com o arquivo de **outro** jogo (hoje: event flags e consultas de módulo no `runtime_tlg.c`, ErrorDialog no `runtime_gr2.c`): esses são os primeiros a subir.
4. **Correções do núcleo** são feitas aqui (ou num port e trazidas com `core_sync.py --pull`), e cada port atualiza o núcleo num PR próprio, testado no seu jogo.

## Configuração em camadas: cada jogo continua configurável

| # | Camada | Onde fica | Exemplos |
|---|---|---|---|
| 1 | **Padrões do núcleo** | código do núcleo (`tlg_settings.cpp`, `runtime_*.c`) | o que vale quando ninguém disse nada |
| 2 | **Perfil do jogo** | no port: `port/packaging/port.env` (identidade: nome, slug, título, ícone, `PORT_ENV_PREFIX`) + `port/profile.ini` (padrões) | modo de readback, orçamento de VRAM (`gc_budget_mb=3500` no inFAMOUS), `block1d`, skip de adjacência, patches de FPS, sensor e touchpad, mapa do mouse, upscaler |
| 3 | **Código do jogo** | `runtime_<slug>*.c` + `game_<slug>.c` + `<slug>_patches.py`, `patches/*.xml` | imports que só o jogo usa, correções no eboot |
| 4 | **Escolhas do jogador** | launcher e menu Insert, salvas **por jogo**: `~/.config/<slug>-launcher/settings.json` e `port/user/<TITLE_ID>/<prefixo>.ini` (hoje `port/user/tlg.ini`) | resolução, FPS, idioma, precisão da GPU, controles |

**Precedência: núcleo < perfil do jogo < jogador.** Variáveis de ambiente valem por cima de tudo, só para testes (`ISS_READBACKS=2 ./run_infamous.sh`).

**Carregador (a implementar):** um só, genérico, no núcleo (`port/src/core_config.c`, com ponte C++ em `gpu/shim/tlg_settings.cpp`); ele não conhece nenhum jogo, o port fornece os valores. Ordem: padrões compilados → `profile.ini` do jogo (ao lado do `tlg-probe`, ou `<PREFIXO>_PROFILE`) → ini do jogador → ambiente. API: `core_config_get/int/bool("gpu.readbacks")`; cada chave tem nome de ini (`gpu.readbacks`) e de variável (`READBACKS` → `<PREFIXO>_READBACKS`, e `TLG_READBACKS` na transição). Entra no começo do `main` do `probe.c` (antes do `runtime_init`) e nos `getenv("TLG_…")` do `tlg_settings.cpp`, do `runtime_pad.c` e da GPU. O `<slug>_run.py` do launcher lê o mesmo `profile.ini` para mostrar os padrões e grava só o que o jogador mudou. O `profile.ini` vai no pacote.

### Prefixo das variáveis de ambiente

Hoje o núcleo lê só `TLG_` (herdado do primeiro port). Plano em duas etapas:

1. **Agora, um prefixo por jogo:** **`TLG_`** (The Last Guardian), **`GR2_`** (Gravity Rush 2), **`ISS_`** (inFAMOUS Second Son). O port declara `PORT_ENV_PREFIX="ISS_"` no seu `port/packaging/port.env`; o `build.sh` passa `-DPORT_ENV_PREFIX="ISS_"` ao compilar o `tlg-probe` (no Windows, `-DPORT_ENV_PREFIX=ISS_` ao CMake), e o `alias_port_environment()` do `probe.c` copia cada `ISS_X` para `TLG_X` no início do `main`; o nome do jogo ganha de um `TLG_X` também definido (os runners põem padrões em `TLG_*`). O `port/run.py` segue a mesma regra para `<PREFIXO>ISA_REWRITE`. Sem `PORT_ENV_PREFIX` (TLG, build só do núcleo) não há alias. Jogador, launcher e scripts usam o prefixo do jogo; o núcleo continua lendo `TLG_X`.
2. **Depois, um prefixo comum `PSCORE_`** para o que é do núcleo, quando todo port consumir o `ps4_core`: o carregador procura `<PREFIXO>_X`, `PSCORE_X` e por fim `TLG_X`, avisando uma vez no log quando usar o nome antigo (vem de `runtime_game.env_prefix`). Nomes internos (`tlggpu`, `tlg-probe`, `TLG_SHADER_CACHE_KEY`) não mudam agora.

### Escolhas por jogo que ainda vivem no launcher

- **TSC:** o padrão é o `rdtsc` do host com a frequência medida; `TLG_TSC=process` usa o relógio do processo (1 GHz, do upstream gow3_pc, para hosts com TSC diferente entre núcleos). Jogo que executa `rdtsc` direto no código (inFAMOUS: 391 sítios) **tem** de usar o do host, senão roda 3–4x rápido. Ideia: detectar no `prepare.py` contando os `rdtsc` do eboot ([KNOWN_ISSUES](docs/KNOWN_ISSUES.md) #15).
- **Readback antecipado + write merge** (`TLG_EAGER_READBACK=1`, `TLG_WRITE_MERGE=1`): desligados no núcleo, ligados por padrão só no inFAMOUS. No fence a GPU já copia os buffers que o jogo lê de volta; escritas da CPU sem falta marcam essas cópias como velhas, para uma cópia antiga não sobrescrever bytes novos do jogo (#16).
- **Readback adiado** (`TLG_DEFERRED_READBACK=1`, só com readback preciso): a thread do jogo espera só as suas cópias. Desligado no núcleo, ligado por padrão no launcher do inFAMOUS. Ligados no núcleo para todos, mas testados só no inFAMOUS: pipelines de compute fora da thread da GPU (`TLG_COMPUTE_ASYNC_BUILD=0` desliga) e envio a cada ~1024 draws (`TLG_SUBMIT_DRAWS=0` desliga) (#17).

## Build

- **Ainda não há build só do núcleo.** O `runtime.c` liga contra os resolvers dos jogos, então `tlg-probe`, `runtime-test` e `sema-test` precisam dos `runtime_<slug>*.c`; o `build.sh` e o `CMakeLists.txt` do Windows pegam `src/runtime*.c` por glob; o empacotamento precisa de `port.env`, ícone e `.rc`. Tudo isso se resolve com a tabela `RuntimeGame` + `game_none.c` ([KNOWN_ISSUES](docs/KNOWN_ISSUES.md) #7, #13).
- **Testar uma mudança do núcleo** hoje = levá-la a um port e compilar lá: `cd <port>/port && flock -x <trava> env TLG_LTO=OFF bash build.sh` (~5 min no primeiro build).
- A biblioteca da GPU (`cmake -S port/gpu`) não depende dos arquivos do jogo no Linux.
- Dependências: Vulkan, SDL3, FFmpeg (libavcodec/libavutil/libswscale), FreeType (o `build.sh` usa `../../tools/freetype-devel` se o sistema não tiver), Zydis; Windows em MSYS2 CLANG64 (`msys2-env.sh`).
- **Cache de shaders:** `~/.local/share/shadPS4/cache/<TITLE_ID>-<chave>`; a chave (`TLG_SHADER_CACHE_KEY`) é o SHA-1 dos fontes do tradutor, calculado pelo CMake. Mesma versão do núcleo = mesma chave em todos os jogos; mudar o tradutor aqui gera chave nova e o cache velho é apagado sozinho. Um port não deve mexer no tradutor localmente.
- **Limpar o cache de shaders:** `port/launcher/shader_cache.py` acha e apaga só o cache do jogo (`<TITLE_ID>`, `<TITLE_ID>-<chave>`, `<TITLE_ID>.zip` em `TLG_GPU_USER_DIR`, `user/`, `user/user/` no Windows e `~/.local/share/shadPS4` no Linux), nunca saves, configurações ou logs. O launcher de cada port tem um botão para isso; na linha de comando, `run.py --clear-shader-cache`.

## Regras

- **Um jogo por vez, uma trava.** O jogo nunca roda junto com um build, com o Ghidra ou com outra instância. Tudo passa pela trava exclusiva: `flock -x /tmp/claude-1000/-home-hayatog-decomp/79f27d1d-d3f5-4a2e-a5f3-511e01385ae2/scratchpad/game.lock <comando>`, incluindo builds e testes do `ps4_core`. Um build por vez (32 GB de RAM, `/tmp` é tmpfs), no máximo dois agentes. O `core_sync.py` e o `list_exports.py` só leem: não precisam da trava.
- **Nunca arquivos da Sony** no git: eboot, prx, sprx, psarc, sfo, plt, arte oficial. Os `link.json`/`analysis.json` dos ports só são lidos pelo `list_exports.py`; nada deles é copiado.
- **Sem remoto, sem push.** Os ports só recebem `git fetch origin` daqui (nunca push, nunca troca de branch).
- Commits como `HayatoG <HayatoG@users.noreply.github.com>` com os trailers `Co-Authored-By` / `Claude-Session` (ver `~/decomp/REGRAS_AGENTES.md`).

## `tools/core_sync.py`

Compara **por conteúdo** os arquivos do `CORE_MANIFEST` com a cópia de um port:

```
tools/core_sync.py --status-all [--fetch]                     # tabela dos três ports contra origin/main
tools/core_sync.py --ref origin/main ~/decomp/tlg_pc          # lista os arquivos diferentes
tools/core_sync.py --diff --ref origin/main ~/decomp/gr2_pc   # com diff (ps4_core -> port)
tools/core_sync.py --pull --ref origin/main ~/decomp/tlg_pc   # traz o que o port tem de mais novo
```

Estados: `port-newer` (a versão daqui está no histórico do port: o port andou), `port-only`, `diverged` (os dois mudaram: mesclar à mão), `core-newer` (a versão do port está no histórico daqui ou no commit fixado de um port do `CORE_SOURCE`: o port está atrás), `core-only`, `submodule`. O `--pull` copia `port-newer` e `port-only` (com `--force`, também `diverged`) só para o working tree: revise com `git diff` e faça o commit à mão, atualizando o `CORE_SOURCE`.

### Estado em 14/10/2026 (`--status-all`, depois da branch `sync/2026-10-14`)

| Port | `origin/main` | Diferentes | Leva do port | Port atrasado |
|---|---|---|---|---|
| tlg_pc | `c8f249b` (PR #18) | 118 | 0 | 85 `core-newer` + 33 `core-only` |
| gr2_pc | `40d0391` (PR #15) | 137 | 0 | 90 `core-newer` + 47 `core-only` |
| infamous_pc | `2b82d72` (PR #15) | 49 | 0 | 33 `core-newer` + 16 `core-only` (SMAA/CAS, mouse, presenter, `PORT_ENV_PREFIX`, join do AvPlayer, correções do TLOU) |

Fora das `main`: as branches `fix/avplayer-stop-join` e `fix/isa-rewrite-sse4a` do GR2 já estão aqui; o NGS2 embutido do TLG (`feat/ngs2-hle`) está na branch `feat/ngs2-hle` do `ps4_core`, em cima desta: `port/src/runtime_ngs2.c` responde ao `libSceNgs2` quando `TLG_NGS2` não é `native` (sem o `libSceNgs2.sprx` do firmware), `TLG_NGS2_TRACE` rastreia o módulo nativo (ver `docs/NGS2_LEVANTAMENTO.md`).

Nenhum port tem trabalho de núcleo que falte aqui. Para levar o núcleo a um port: copiar os arquivos `core-newer`/`core-only` numa branch do port, compilar e testar o jogo (com a trava), e abrir o PR.

## Ferramentas

- `tools/list_exports.py -o docs/LIBRARIES.md`: regenera o catálogo de bibliotecas (tabelas `RuntimeExport`, resolvers por NID, `import_names.inc`, `LIB_FUNCTION` da GPU, arquivos de cada jogo e os `link.json` dos ports). Rode depois de mexer num `runtime_*.c`.
- `scripts/sce_imports.py`: imports de um eboot por biblioteca, para um jogo novo.

---

<a id="english"></a>

# ps4_core — shared core of the PS4 native ports

> **Local-only repository.** No GitHub, no remote, no push (for now). Everything a PS4 game needs to run natively on PC accumulates here; today it is copied into each port: [tlg_pc](https://github.com/HayatoG/tlg_pc) (The Last Guardian), [gr2_pc](https://github.com/HayatoG/gr2_pc) (Gravity Rush 2) and `infamous_pc` (inFAMOUS Second Son, GitHub `iss_pc`).

By **HayatoG** · GPL-2.0-or-later · **No Sony file goes in** (eboot, prx, sprx, psarc, sfo, official art).

## What it is

A native port runs the game's own executable (the decrypted `eboot.bin` from the player's dump) directly on the PC's x86-64 CPU. The core provides:
1. **Loader and linker** (`probe.c`, `link_modules.py`, `link_libc.py`): map the eboot and the modules shipped with the game, relocate, bind every `NID#library` import to a host function, fix PC differences (`fs:[0]` → `gs`).
2. **HLE of the PS4 system libraries** in C (`port/src/runtime_*.c`): libkernel, Pad, AudioOut, Ajm, SaveData, UserService, SystemService, Rtc, AppContent, Sysmodule, PlayGo, Videodec, offline NpManager/Trophy, Http/Ssl/Net stubs, dialogs… Full per-function catalogue (real vs stub, which games use it): **[docs/LIBRARIES.md](docs/LIBRARIES.md)**.
3. **GPU layer** (`port/gpu/`): shadPS4 fork translating GCN command buffers (libSceGnmDriver) and shaders to Vulkan, plus VideoOut, AvPlayer and `shim/` (SDL3 window, overlay, Insert menu, upscalers, post-processing).

Ports keep only their game: `runtime_<slug>*.c`, patches, profile, launcher, icon, docs.

## Layout

See the tree above. `CORE_MANIFEST` lists what is core (include globs, `!` excludes, `@` submodules) and drives `core_sync.py`; `CORE_SOURCE` records which port commit each part came from. Paths under `port/` and `scripts/` are identical to a port repo; root `docs/` and `tools/` belong to ps4_core only. Submodules: same commits as infamous `main`, upstream URLs in `.gitmodules`.

## Core vs per game

Per game: `runtime_<slug>*.c`, future `game_<slug>.c` and `profile.ini`, `<slug>_patches.py`, `port.env`, icon, `README-player.md`, `windows/*.rc`, launchers, run/test scripts, README, docs, CI, `dump/`, `game/`. **`import_names.inc` is a union** of every game's imports (`runtime_lookup` resolves by name; a missing name is an unlinked function, an extra one costs nothing).

## How ports consume it

Today each port carries a copy kept in step with `tools/core_sync.py`. Plan: (1) the core as a submodule at `port/core`, thin port build files; (2) game hooks through a table, draft in **`port/src/runtime_game.h`** (commented, not wired): slug, name, title IDs, env prefix, profile, ordered resolvers, `init`, `patch_image`, `report`, defined by the port in `game_<slug>.c`, with an empty `game_none.c` for core-only builds; (3) functions two games use move into the core — `docs/LIBRARIES.md` "Coverage per game" lists the imports a game answers with *another* game's file (event flags in `runtime_tlg.c`, ErrorDialog in `runtime_gr2.c`); (4) core fixes land here and each port bumps the core in its own tested PR.

## Layered configuration

1. **Core defaults** (core code). 2. **Game profile** (`port.env` identity + `PORT_ENV_PREFIX`, `profile.ini` defaults: readback mode, VRAM budget, `block1d`, skip-adjacency, FPS patches, motion/touchpad, mouse map, upscaler). 3. **Game code** (`runtime_<slug>*.c`, `game_<slug>.c`, patches). 4. **Player choices** (launcher and Insert menu, saved per game). **Precedence: core < game profile < player**; environment variables override everything, for testing only. One generic loader in the core (`core_config.c` + C++ bridge), knowing no game: compiled defaults → game `profile.ini` → player ini → environment; `core_config_get/int/bool("gpu.readbacks")`, env name `<PREFIX>_READBACKS` then `TLG_READBACKS` during the transition.

**Env prefix plan:** now per game — `TLG_` (The Last Guardian), `GR2_` (Gravity Rush 2), `ISS_` (inFAMOUS Second Son): the port sets `PORT_ENV_PREFIX="ISS_"` in `port/packaging/port.env`, `build.sh` compiles `tlg-probe` with `-DPORT_ENV_PREFIX="ISS_"` (CMake option on Windows), and `probe.c` `alias_port_environment()` copies each `ISS_X` to `TLG_X` at start, the game's name winning over a `TLG_X` also set (runners set `TLG_*` defaults); `port/run.py` does the same for `<PREFIX>ISA_REWRITE`; no define = no alias. Later a common `PSCORE_` prefix for core settings once every port consumes ps4_core (lookup `<PREFIX>_X`, `PSCORE_X`, then `TLG_X`, old name logged once). Internal names (`tlggpu`, `tlg-probe`, `TLG_SHADER_CACHE_KEY`) stay.

**Per-game choices still in the launchers:** TSC — host rdtsc with measured frequency by default, `TLG_TSC=process` for the process clock; games that execute rdtsc directly (inFAMOUS, 391 sites) must use the host rdtsc (idea: auto-detect by counting rdtsc in the eboot at prepare time; KNOWN_ISSUES #15). Eager readback + write merge (`TLG_EAGER_READBACK`, `TLG_WRITE_MERGE`) are off in the core and on by default only in inFAMOUS; CPU writes merged without a fault mark the eager copies stale so an old copy never overwrites the game's new bytes (#16). Deferred readback (`TLG_DEFERRED_READBACK=1`, precise mode only) is off in the core and on in the inFAMOUS launcher; compute pipelines off the GPU thread (`TLG_COMPUTE_ASYNC_BUILD=0` off) and submit every ~1024 draws (`TLG_SUBMIT_DRAWS=0` off) are on in the core but tested only in inFAMOUS (#17).

## Build notes

No core-only build yet (runtime.c links the per-game resolvers; build globs `src/runtime*.c`; packaging needs a game profile) — solved by the `RuntimeGame` table + `game_none.c` (KNOWN_ISSUES #7, #13). To test a core change, bring it into a port and build there under the lock. The GPU library (`cmake -S port/gpu`) does not need game files on Linux. Dependencies: Vulkan, SDL3, FFmpeg, FreeType, Zydis; Windows via MSYS2 CLANG64. Shader cache `~/.local/share/shadPS4/cache/<TITLE_ID>-<key>`, key = SHA-1 of the recompiler sources: same core, same key in every game; a recompiler change invalidates old caches automatically. `launcher/shader_cache.py` clears one game's cache (launcher button; `run.py --clear-shader-cache`).

## Rules

- **One game at a time, one lock:** the game never runs with a build, Ghidra or another instance; everything (ps4_core builds and tests included) goes through `flock -x /tmp/claude-1000/-home-hayatog-decomp/79f27d1d-d3f5-4a2e-a5f3-511e01385ae2/scratchpad/game.lock`; one build at a time; at most two agents. `core_sync.py` and `list_exports.py` only read and need no lock.
- **Never Sony files** in git. The ports' `link.json`/`analysis.json` are only read by `list_exports.py`.
- **No remote, no push.** Ports only get `git fetch origin` from here (never push, never a branch switch).
- Commits as `HayatoG <HayatoG@users.noreply.github.com>` with the `Co-Authored-By` / `Claude-Session` trailers.

## `tools/core_sync.py`

`--status-all [--fetch]` (table of the three ports vs `origin/main`), `--ref`, `--diff`, `--pull` (copies `port-newer` and `port-only`, plus `diverged` with `--force`, into the working tree for review). States: `port-newer`, `port-only`, `diverged`, `core-newer` (port behind; checked against this history and the pinned commits in `CORE_SOURCE`), `core-only`, `submodule`. Status on 2026-10-14 after the `sync/2026-10-14` branch (infamous_pc `2b82d72`, gr2_pc branches, `feat/tlou-imports` fixes): no port's `main` has core work missing here; tlg_pc `c8f249b` 118 behind (85 core-newer, 33 core-only), gr2_pc `40d0391` 137 behind (90 + 47), infamous_pc `2b82d72` 49 behind (33 + 16: SMAA/CAS, mouse, presenter, `PORT_ENV_PREFIX`, AvPlayer join, TLOU fixes; inFAMOUS must add `PORT_ENV_PREFIX="ISS_"` to its `port.env` when it takes this core). Still outside: tlg_pc `feat/ngs2-hle` (ps4_core branch `feat/ngs2-hle`).

## Tools

`tools/list_exports.py -o docs/LIBRARIES.md` regenerates the library catalogue; `scripts/sce_imports.py` lists a new game's imports per library.
