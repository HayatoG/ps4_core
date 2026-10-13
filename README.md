<h1 align="center">ps4_core — núcleo compartilhado dos ports nativos de PS4</h1>

<p align="center">
  <a href="#english"><b>English below</b></a> ·
  <a href="https://www.youtube.com/@hayatog-dev">YouTube</a> ·
  <a href="https://discord.gg/UMkdMYZh9g">Discord</a>
</p>

> **Repositório só local.** Não tem GitHub nem push. Ele junta num lugar só o núcleo que hoje vai copiado em cada port: [tlg_pc](https://github.com/HayatoG/tlg_pc) (The Last Guardian), [gr2_pc](https://github.com/HayatoG/gr2_pc) (Gravity Rush 2) e `infamous_pc` (inFAMOUS Second Son, GitHub `iss_pc`).

Por **HayatoG** · Licença GPL-2.0-or-later (ver `port/LICENSE` e `port/UPSTREAM.md`). **Nenhum arquivo da Sony entra aqui** (eboot, prx, sprx, psarc, sfo, arte oficial): o `.gitignore` barra os formatos.

## O que tem aqui

O conteúdo veio da `main` do `infamous_pc` (o núcleo mais novo), com o commit registrado em `CORE_SOURCE`. Foi copiado com `git archive`, então só entraram arquivos versionados. Os caminhos são **os mesmos** de um repositório de port, para que copiar e comparar seja direto.

| Caminho | O que é |
|---|---|
| `port/src/` | loader (`probe.c`) e runtime nativo (`runtime_*.c` do sistema: kernel, memória, threads, áudio, Ajm, pad, saves, arquivos…), `import_names.inc`, `compat_win.*`, os hooks do menu Insert (`*_hook.h`) |
| `port/scripts/` | `prepare.py`, `link_modules.py`, `link_libc.py`, `content_profile.py`, `patches.py`, `mods.py`, `graphics_options.py`, `save_backup.py` |
| `port/gpu/` | fork do shadPS4 (`shadps4/`), `shim/` (janela, overlay, menu Insert, configurações), `CMakeLists.txt`, `patches/`, `dlss_bridge/`, `third_party/` |
| `port/third_party/LibAtrac9` | submódulo |
| `port/build.sh`, `run.sh`, `run.py`, `msys2-env.sh`, `shell.nix` | build e execução |
| `port/tests/`, `port/tools/`, `port/docs/` | testes, ferramentas (FSR4, captura, soak) e documentos técnicos da GPU |
| `port/packaging/` | empacotamento genérico (Linux, Windows, AppImage, `check_package.sh`) |
| `port/launcher/tlg_vulkan.py` | escolha de ICD Vulkan, igual em todos |
| `scripts/` | ferramentas genéricas: `make_game_view.py`, `session_chart.py`, `sce_imports.py`, `psarc_toc.py`, FTP/USB, `watch_session.sh` |
| `CORE_MANIFEST` | **a lista do que é núcleo** (globs, exclusões e submódulos). É a fonte de verdade do `core_sync.py` |
| `tools/core_sync.py` | compara o núcleo com a cópia de um port e traz arquivos novos para cá |

### Submódulos

Os sete submódulos do `infamous_pc` (`fsr-vulkan`, `imgui`, `magic_enum`, `miniz`, `vulkan-headers`, `xbyak` em `port/gpu/third_party/`, e `port/third_party/LibAtrac9`) foram adicionados **sem rede**: clonados dos checkouts locais (`infamous_pc/.git/modules/...`), postos no mesmo commit da `main` do inFAMOUS, e o `.gitmodules` voltou às URLs oficiais (GitHub). Num clone novo deste repositório basta `git submodule update --init` (com rede) ou, offline, `git -c protocol.file.allow=always submodule update --init` depois de apontar `submodule.<nome>.url` para um checkout local com `git config`.

## O que é núcleo e o que é de cada jogo

| Núcleo (`ps4_core`) | De cada jogo (fica no port) |
|---|---|
| tudo de `port/src/` menos o que está à direita | `port/src/runtime_<slug>*.c`: `runtime_tlg.c`, `runtime_gr2*.c`, `runtime_infamous.c` |
| `import_names.inc` (**união** de todos os jogos, ver abaixo) | `port/src/game_<slug>.c` (futuro): a tabela de hooks do jogo |
| scripts de preparação e link, `patches.py` (o mecanismo) | tabelas de patch do jogo: `port/scripts/<slug>_patches.py` (ex.: `tlg_patches.py`, FPS do TLG) |
| GPU inteira (shadPS4 + shim), submódulos | `port/profile.ini` (futuro): os padrões do jogo |
| empacotamento genérico | `port/packaging/port.env`, `icon/`, `README-player.md`, `windows/*.rc` (apontam para o ícone) |
| `tlg_vulkan.py` | launchers `port/launcher/<slug>_launcher.py`, `<slug>_run.py`, `<slug>.png/.ico`; `<slug>_launcher.sh`, `run_<slug>.sh` |
| ferramentas genéricas de `scripts/` | `scripts/<slug>_run.sh`, `newgame_test.sh` e roteiros de teste do jogo |
| | README, `docs/`, `.github/workflows` (igual, com nomes trocados), `dump/`, `game/` |

**`import_names.inc`: união.** A tabela NID → nome não é só diagnóstico: `runtime_lookup` resolve imports pelo nome, então um nome faltando é uma função que não liga. Uma entrada a mais não custa nada (o jogo que não importa aquele NID nunca procura por ele). Por isso o núcleo guarda **uma tabela só, com os imports de todos os jogos**. Um jogo novo acrescenta as suas linhas aqui (gerar com `scripts/sce_imports.py`), nunca numa cópia do port.

## Como um port vai usar o núcleo

1. **Submódulo em `port/core`.** O port deixa de ter a cópia e passa a ter `port/core` → `ps4_core` (quando houver remoto; até lá, caminho local). O `build.sh` e o `CMakeLists.txt` do port ficam finos: incluem `core/port/gpu` e compilam `core/port/src/*.c` + os `src/runtime_<slug>*.c` + `src/game_<slug>.c` do próprio port.
2. **Hooks do jogo por tabela, não por nome fixo.** Hoje o `runtime.c` chama `runtime_tlg_resolve`, `runtime_gr2_*_resolve` e `runtime_infamous_resolve` diretamente, então todo port precisa ter todos esses arquivos (é por isso que o gr2_pc ainda carrega o `runtime_tlg.c`). O plano é um cabeçalho do núcleo, `runtime_game.h`:
   ```c
   typedef struct RuntimeGame {
       const char *slug;            /* "infamous" */
       const char *env_prefix;      /* "ISS" (TLG_ continua aceito na transição) */
       const char *const *title_ids;/* {"CUSA00223", NULL} */
       uintptr_t (*const *resolvers)(const char *name); /* em ordem; NULL no fim */
       void (*init)(void);          /* depois de carregar a configuração (pode ser NULL) */
       void (*report)(void);        /* relatório no fim (pode ser NULL) */
   } RuntimeGame;
   extern const RuntimeGame runtime_game; /* definido pelo port em src/game_<slug>.c */
   ```
   `runtime_resolve` percorre primeiro os módulos do sistema (núcleo), depois `runtime_game.resolvers` na ordem que o jogo escolher (o GR2 põe o Audio3d antes dos placeholders), e por fim a GPU. Funções que **mais de um jogo** precisa sobem para o núcleo (`runtime_services.c` etc.); o `runtime_<slug>.c` fica só com o que é do jogo.
3. **Correções do núcleo** são feitas aqui (ou no port e trazidas com `core_sync.py --pull`), e cada port atualiza o submódulo num PR próprio, testado no seu jogo.

### Configuração em camadas: cada jogo continua configurável

Depois da separação, cada jogo segue com os seus próprios padrões e as escolhas do jogador. São quatro camadas:

| # | Camada | Onde fica | Exemplos |
|---|---|---|---|
| 1 | **Padrões do núcleo** | código do núcleo (`tlg_settings.cpp`, `runtime_*.c`) | o que vale quando ninguém disse nada |
| 2 | **Perfil do jogo** | no port: `port/packaging/port.env` (identidade: nome, slug, título, ícone, `PORT_ENV_PREFIX`) + `port/profile.ini` (padrões) | modo de readback da GPU, orçamento de VRAM (`gc_budget_mb=3500` no inFAMOUS), `block1d`, skip de adjacência, patches de FPS, sensor de movimento e touchpad (botão, zona morta), upscaler |
| 3 | **Código do jogo** | `port/src/runtime_<slug>*.c` + tabela `game_<slug>.c` + patches do jogo (`<slug>_patches.py`, `patches/*.xml`) | imports que só o jogo usa, correções no eboot |
| 4 | **Escolhas do jogador** | launcher e menu Insert, salvas **por jogo**: `~/.config/<slug>-launcher/settings.json` e `port/user/<TITLE_ID>/<prefixo>.ini` (hoje `port/user/tlg.ini`) | resolução, FPS, idioma, precisão da GPU, controles |

**Precedência: núcleo < perfil do jogo < jogador.** Variáveis de ambiente continuam valendo por cima de tudo, só para testes e desenvolvimento (`ISS_READBACKS=2 ./run_infamous.sh`).

**Como o núcleo lê o perfil (a implementar):** um carregador genérico só, no núcleo (`port/src/core_config.c`, com uma ponte C++ em `gpu/shim/tlg_settings.cpp`). Ele não conhece nenhum jogo: o port fornece os valores.
- Ordem de leitura: tabela de padrões compilada no núcleo → `profile.ini` do jogo (ao lado do `tlg-probe`, ou em `<PREFIXO>_PROFILE`) → ini do jogador → ambiente.
- API: `core_config_get("gpu.readbacks")`, `core_config_int(...)`, `core_config_bool(...)`. Cada chave tem um nome de ini (`gpu.readbacks`) e um nome de variável (`READBACKS`), procurado como `<PREFIXO>_READBACKS` e, na transição, `TLG_READBACKS`.
- Onde entra: no começo do `main` do `probe.c` (antes de `runtime_init`, que já lê `TLG_*`), e nos `getenv("TLG_…")` do `tlg_settings.cpp`, `runtime_pad.c` e da GPU, que passam a chamar o carregador. O `<slug>_run.py` do launcher lê o mesmo `profile.ini` para mostrar os padrões do jogo na tela, e grava só o que o jogador mudou.
- O `profile.ini` vai dentro do pacote; o `check_package.sh` não muda.

### Prefixo das variáveis de ambiente

Hoje todo port usa `TLG_` (herdado do primeiro port). O plano: cada jogo com o seu prefixo, **`TLG_`** (The Last Guardian), **`GR2_`** (Gravity Rush 2), **`ISS_`** (inFAMOUS Second Son), vindo de `PORT_ENV_PREFIX` no `port.env` / `runtime_game.env_prefix`. O carregador procura primeiro `<PREFIXO>_X` e depois `TLG_X`, avisando uma vez no log quando usar o nome antigo. Os nomes internos do código (`tlggpu`, `tlg-probe`, `TLG_SHADER_CACHE_KEY`) não mudam agora.

### Cache de shaders

Fica em `~/.local/share/shadPS4/cache/<TITLE_ID>-<chave>`. A chave (`TLG_SHADER_CACHE_KEY`) é o SHA-1 dos fontes do tradutor (`shadps4/shader_recompiler/**`, `amdgpu/resource.h`, `vk_pipeline_cache.cpp`), calculado pelo CMake a cada build. Com o núcleo compartilhado:
- a mesma versão do núcleo gera a **mesma chave** em todos os jogos; o `<TITLE_ID>` separa os jogos;
- mudar o tradutor aqui gera chave nova, e o cache antigo do jogo é apagado sozinho;
- um port **não** deve mexer no tradutor localmente; se mexer, a chave muda junto (o hash é dos arquivos compilados), então o cache nunca fica velho.

### Um jogo por vez e a trava

As regras de `~/decomp/REGRAS_AGENTES.md` valem aqui também:
- o jogo nunca roda junto com um build, com o Ghidra ou com outra instância do jogo;
- **tudo** passa pela mesma trava exclusiva: `flock -x /tmp/claude-1000/-home-hayatog-decomp/79f27d1d-d3f5-4a2e-a5f3-511e01385ae2/scratchpad/game.lock <comando>`, incluindo os builds e testes do `ps4_core`;
- um build por vez (RAM: 32 GB, e o `/tmp` é tmpfs); no máximo dois agentes em paralelo;
- o `core_sync.py` só lê e copia arquivos: não precisa da trava.

## `tools/core_sync.py`

Enquanto os ports ainda carregam a sua cópia, o núcleo é mantido em paralelo com esta ferramenta. Ela compara **por conteúdo** os arquivos do `CORE_MANIFEST`:

```
tools/core_sync.py ~/decomp/tlg_pc                  # diferenças contra o working tree do port
tools/core_sync.py --ref main ~/decomp/gr2_pc       # contra uma branch ou commit do port
tools/core_sync.py --diff --ref main ~/decomp/gr2_pc  # com diff unificado (ps4_core -> port)
tools/core_sync.py --pull --ref main ~/decomp/infamous_pc  # traz para cá o que o port tem de mais novo
tools/core_sync.py --summary --ref main ~/decomp/{tlg,gr2,infamous}_pc
```

Cada arquivo diferente recebe um estado: `port-newer` (a versão daqui está no histórico do port, ou seja, o port andou), `core-newer` (a versão do port está no histórico daqui ou da linhagem em `CORE_SOURCE`: o port está atrás), `diverged` (os dois mudaram), `port-only`, `core-only` e `submodule` (commit diferente). O `--pull` copia `port-newer` e `port-only` (com `--force`, também `diverged`) só para o working tree: revise com `git diff` e faça o commit à mão. Submódulos nunca são copiados; a ferramenta mostra o comando.

### Estado em 13/10/2026

| Port | Ref | Diferentes | Detalhe |
|---|---|---|---|
| infamous_pc | `main` (`4448ef2`) | 0 | origem do núcleo |
| tlg_pc | `main` local (`183ab8d`, antes do PR #12) | 47 | 45 atrás, 1 só aqui (`make_game_view.py`), 1 divergente (só um comentário no `vk_scheduler.cpp`) |
| gr2_pc | `main` local (`6aca2ca`, antes do PR #9) | 39 | 38 atrás, 1 só aqui (`make_game_view.py`) |
| tlg_pc | `origin/main` (`f3eece1`, PR #12 mesclado) | 57 | 14 atrás, 11 divergentes, 15 mais novos no TLG, 16 só no TLG: o pós-processamento SMAA/CAS (`postfx_pass`, `smaa*`, `cas*`), o mouse como analógico (`mouse_stick.h`) e ajustes no presenter e nas configurações. A trazer num `--pull` revisado, junto com os divergentes (`build.sh`, `CMakeLists.txt`, `probe.c`, `runtime_pad.c`, `tlggpu.cpp`…) |
| gr2_pc | `origin/main` (`a0fd165`, PR #9 mesclado) | 10 | 9 atrás (a correção wave64 `ReadFirstLane`, `runtime.c`/`runtime.h`/`import_names.inc` com o inFAMOUS, scripts de empacotamento e de atalho), 1 divergente (`probe.c`) |

## Build sozinho (ainda não)

O `ps4_core` sozinho ainda não gera o `tlg-probe`. Falta:
- **a tabela de hooks** (`runtime_game.h`): o `runtime.c`/`runtime.h` chamam `runtime_tlg_resolve`, `runtime_gr2*_resolve`, `runtime_infamous_resolve` e `runtime_gr2_audio3d_report`, que ficam nos arquivos do jogo; sem eles o link falha (o `tlg-probe`, o `runtime-test` e o `sema-test`). Com a tabela, um `src/game_none.c` vazio basta para um build e testes só do núcleo;
- o `CMakeLists.txt` (Windows) e o `build.sh` pegam `src/runtime*.c` por glob: precisam aceitar uma pasta de fontes do jogo;
- `port.env`, ícone e os `.rc` do Windows são do jogo: o empacotamento precisa de um perfil (ou um genérico) para rodar;
- restos de outros jogos para limpar: `run.sh`/`run.py`/`patches.py` com `CUSA01623` (God of War III) como padrão, `tools/tlg_frame_map.py` (GoW3).

A biblioteca da GPU (`cmake -S port/gpu`) não depende dos arquivos do jogo no Linux e deve configurar sozinha; não foi compilada aqui para não disputar a trava.

---

<a id="english"></a>

# ps4_core — shared core of the PS4 native ports

> **Local-only repository.** No GitHub, no push. It holds in one place the core that today is copied into each port: [tlg_pc](https://github.com/HayatoG/tlg_pc) (The Last Guardian), [gr2_pc](https://github.com/HayatoG/gr2_pc) (Gravity Rush 2) and `infamous_pc` (inFAMOUS Second Son, GitHub `iss_pc`).

By **HayatoG** · GPL-2.0-or-later (see `port/LICENSE`, `port/UPSTREAM.md`). **No Sony file goes in** (eboot, prx, sprx, psarc, sfo, official art); `.gitignore` blocks those formats.

## Contents

Imported from `infamous_pc` `main` (the newest core; commit in `CORE_SOURCE`) with `git archive`, so only tracked files came in. Paths are **identical** to a port repo, so copying and comparing is direct. `CORE_MANIFEST` is the list of what counts as core (globs, excludes, submodules) and the source of truth for `tools/core_sync.py`.

Core: `port/src/` (loader `probe.c`, system runtime `runtime_*.c`, `import_names.inc`, `compat_win.*`, Insert-menu hooks), `port/scripts/` (prepare/link/content/patch/mod scripts), `port/gpu/` (shadPS4 fork, `shim/`, CMake, patches, third_party), `port/third_party/LibAtrac9`, `port/build.sh`/`run.sh`/`run.py`/`msys2-env.sh`/`shell.nix`, `port/tests`, `port/tools`, `port/docs`, generic `port/packaging`, `port/launcher/tlg_vulkan.py`, generic `scripts/` tools.

Submodules (`fsr-vulkan`, `imgui`, `magic_enum`, `miniz`, `vulkan-headers`, `xbyak`, `LibAtrac9`) were added **offline** from the local checkouts in `infamous_pc/.git/modules/...`, pinned to the commits of infamous `main`, and `.gitmodules` was rewritten back to the upstream GitHub URLs. In a fresh clone: `git submodule update --init` (network), or point `submodule.<name>.url` at a local checkout and use `git -c protocol.file.allow=always submodule update --init`.

## Core vs per game

Per game (stays in the port): `port/src/runtime_<slug>*.c`, the future `port/src/game_<slug>.c` hook table, `port/scripts/<slug>_patches.py` (e.g. TLG FPS patches), the future `port/profile.ini`, `port/packaging/port.env`, `icon/`, `README-player.md`, `windows/*.rc`, launchers (`port/launcher/<slug>_*`, `<slug>_launcher.sh`, `run_<slug>.sh`), `scripts/<slug>_run.sh` and game test scripts, README, `docs/`, CI workflow, `dump/`, `game/`.

**`import_names.inc` is a union.** The NID → name table is not only diagnostics: `runtime_lookup` resolves imports by name, so a missing name is an unlinked function, while an extra entry costs nothing. The core keeps **one table with every game's imports**; a new game adds its lines here (generate with `scripts/sce_imports.py`).

## How a port will consume the core

1. **Submodule at `port/core`.** The port drops its copy; its `build.sh`/`CMakeLists.txt` include `core/port/gpu` and compile `core/port/src/*.c` plus the port's own `src/runtime_<slug>*.c` and `src/game_<slug>.c`.
2. **Game hooks through a table.** Today `runtime.c` calls `runtime_tlg_resolve`, `runtime_gr2_*_resolve` and `runtime_infamous_resolve` by name, so every port must carry all of them (that is why gr2_pc still carries `runtime_tlg.c`). Plan: a core header `runtime_game.h` with `struct RuntimeGame { slug, env_prefix, title_ids, resolvers[] (ordered, NULL-terminated), init, report }` and `extern const RuntimeGame runtime_game;` defined by the port. `runtime_resolve` walks the core system modules, then the game's resolvers in the game's order, then the GPU. Functions needed by more than one game move into the core.
3. Core fixes are made here (or in a port and brought in with `core_sync.py --pull`); each port bumps the submodule in its own tested PR.

### Layered configuration: every game stays configurable

1. **Core defaults** — in core code.
2. **Game profile** — in the port: `port/packaging/port.env` (identity, `PORT_ENV_PREFIX`) + `port/profile.ini` (defaults: GPU readback mode, VRAM budget, `block1d`, skip-adjacency, FPS patches, motion/touchpad mapping, upscaler…).
3. **Game code** — `runtime_<slug>*.c`, `game_<slug>.c`, game patches.
4. **Player choices** — launcher and Insert menu, saved **per game**: `~/.config/<slug>-launcher/settings.json` and `port/user/<TITLE_ID>/<prefix>.ini` (today `port/user/tlg.ini`).

**Precedence: core < game profile < player**; environment variables still override everything, for testing only. **Loader (to be implemented):** one generic loader in the core (`port/src/core_config.c` plus a C++ bridge in `gpu/shim/tlg_settings.cpp`) that knows no game; the port supplies the values. Reads compiled defaults → game `profile.ini` (next to `tlg-probe` or `<PREFIX>_PROFILE`) → player ini → environment. API `core_config_get/int/bool("gpu.readbacks")`; each key also has an env name looked up as `<PREFIX>_READBACKS`, then `TLG_READBACKS` during the transition. Hook points: start of `probe.c` `main` (before `runtime_init`), and the `getenv("TLG_…")` calls in `tlg_settings.cpp`, `runtime_pad.c` and the GPU. The launcher's `<slug>_run.py` reads the same `profile.ini` to show the game's defaults and saves only what the player changed. `profile.ini` ships inside the package.

### Environment prefix

Today every port uses `TLG_`. Plan: `TLG_` (The Last Guardian), `GR2_` (Gravity Rush 2), `ISS_` (inFAMOUS Second Son), from `PORT_ENV_PREFIX` / `runtime_game.env_prefix`; the loader tries `<PREFIX>_X` first, then `TLG_X` (logging the old name once). Internal names (`tlggpu`, `tlg-probe`, `TLG_SHADER_CACHE_KEY`) stay for now.

### Shader cache

`~/.local/share/shadPS4/cache/<TITLE_ID>-<key>`; the key is the SHA-1 of the recompiler sources, computed by CMake on every build. Same core version → same key in every game, separated by title ID; a recompiler change here makes a new key and the old cache is deleted automatically; a port should not patch the recompiler locally, and if it does, the key follows the compiled files.

### One game at a time, one lock

Same rules as `~/decomp/REGRAS_AGENTES.md`: the game never runs together with a build, Ghidra or another game instance; everything — including ps4_core builds and tests — goes through `flock -x /tmp/claude-1000/-home-hayatog-decomp/79f27d1d-d3f5-4a2e-a5f3-511e01385ae2/scratchpad/game.lock`; one build at a time; at most two agents. `core_sync.py` only reads/copies files and needs no lock.

## `tools/core_sync.py`

Compares the `CORE_MANIFEST` files **by content** between ps4_core and a port (working tree, or `--ref <branch>`); `--diff` shows a unified diff, `--pull` copies the port's `port-newer` and `port-only` files (plus `diverged` with `--force`) into the ps4_core working tree for review, `--summary` gives one line per port. States: `port-newer`, `core-newer` (port is behind; checked against the history here and the lineage in `CORE_SOURCE`), `diverged`, `port-only`, `core-only`, `submodule`. See the table above for the 13/10/2026 snapshot: infamous `main` 0; tlg local `main` 47 and gr2 local `main` 39 (behind, before PRs #12/#9); after the merges, tlg `origin/main` 57 (TLG has new core work: SMAA/CAS post-processing, mouse stick — to be pulled) and gr2 `origin/main` 10 (9 behind, `probe.c` diverged).

## Standalone build (not yet)

Missing: the `runtime_game.h` hook table (today `runtime.c` links against the per-game resolvers, so `tlg-probe`, `runtime-test` and `sema-test` fail without them; an empty `game_none.c` would then do), a game-sources folder in `build.sh`/Windows CMake instead of the `src/runtime*.c` glob, a packaging profile (`port.env`, icon, `.rc`), and cleanup of God of War III leftovers (`CUSA01623` defaults in `run.sh`/`run.py`/`patches.py`, `tools/tlg_frame_map.py`). The GPU library (`cmake -S port/gpu`) does not depend on game files on Linux; it was not built here to avoid taking the lock.
