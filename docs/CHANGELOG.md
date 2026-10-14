# Changelog do núcleo / Core changelog

[English below](#english). Hashes da `main` do port onde a mudança nasceu (`iss` = infamous_pc, `gr2` = gr2_pc, `tlg` = tlg_pc). O `ps4_core` só junta: os commits daqui dizem de qual snapshot cada arquivo veio (ver também `CORE_SOURCE`).

## 14/10/2026: sync com o `infamous_pc` `fd78ad5` (PRs #5 e #6)

Do inFAMOUS desde `f76850a`: readback antecipado, ajustes do write merge, prefixo `ISS_` e o gráfico de sessão. O resto do PR #6 é o mesmo sync do upstream gow3_pc que já estava aqui (mesclas em 3 vias com base `iss f76850a`; o lado do núcleo — SMAA/CAS, mouse e teclado, presenter, `TLG_SKIP_ADJACENCY`, upstream — ficou inteiro).

**GPU**
- **Readback antecipado** (`TLG_EAGER_READBACK=1`, desligado no núcleo; o inFAMOUS liga por padrão no launcher e no `infamous_run.sh`): nos pacotes de fence (`liverpool.cpp` → `Rasterizer::EagerCopyAtFence`) a GPU copia para a CPU os buffers que o jogo costuma ler de volta, e a falta de leitura seguinte é atendida pela cópia sem esperar a GPU (`buffer_cache.cpp`, `TryEagerRead` em `vk_rasterizer.cpp`). `TLG_READBACK_STATS=1` conta readbacks por faixa (`iss c5dd761`).
- **Cópias velhas descartadas** (`iss f4c30a1`): bytes que a CPU escreve sem falta (`TLG_WRITE_MERGE`) e o upload seguinte mudam a cópia da GPU depois do fence; as cópias antecipadas dessas páginas viram *stale* (`MarkEagerStale`/`NoteGpuWriteForEager`). Antes, servidas juntas, uma cópia velha sobrescrevia os bytes novos do jogo (objetos sumiam quando uma base da D.U.P. explodia). Também não se serve a cópia depois de um readback de imagem da mesma faixa (a imagem resolveu dados mais novos).

**Runtime e loader**
- **Prefixo de ambiente por jogo** (`iss d62c942`, genérico aqui): `probe.c` `alias_port_environment()` copia cada `<PREFIXO>X` para `TLG_X` no início (se `TLG_X` não estiver definido). O prefixo vem do port: `PORT_ENV_PREFIX="ISS_"` no `port/packaging/port.env` (ou no ambiente); o `build.sh` passa `-DPORT_ENV_PREFIX="ISS_"` ao `tlg-probe` (no Windows, `-DPORT_ENV_PREFIX=` ao CMake). Sem a definição (TLG, build só do núcleo) não há alias. **O `infamous_pc` precisa acrescentar `PORT_ENV_PREFIX="ISS_"` ao seu `port.env` ao trazer este núcleo** (antes o `ISS_` era fixo no `probe.c`).
- **TSC** (`1bf317a` aqui, `iss d62c942`): o padrão volta a ser o `rdtsc` do host com a frequência medida; o relógio do processo do upstream (1 GHz) fica opcional com `TLG_TSC=process`. O inFAMOUS executa `rdtsc` direto em 391 lugares e rodava 3–4x rápido com o relógio do processo (ver KNOWN_ISSUES #15).

**Scripts**
- `scripts/session_chart.py`: modo sem limite de FPS (`iss e54913e`).

**Testes**
- `test_pad.c`/`test_pad_map.c`: stub de `tlggpu_overlay_notice` (o `runtime_pad.c` mesclado avisa na tela ao trocar o modo do sensor; o `build.sh --test` não ligava). `test_gpu_runtime_stubs.cpp`: stub de `runtime_report` (o `tlggpu.cpp` do upstream chama; `motion-shader-test` e afins não ligavam).
- Conferido num worktree do `infamous_pc` `fd78ad5` com este núcleo: `build.sh` e `build.sh --test` passam. `camera-motion-test` e `postfx-test` caem dentro de `Vulkan::CreateInstance` (ponteiro nulo); o `camera-motion-test` cai igual na `main` do inFAMOUS (lá nem ligava sem os stubs): problema anterior dos testes de GPU sem janela.

## 13/10/2026: upstream gow3_pc (branch `upstream/gow3-sync`)

Do [FelipePrado0/gow3_pc](https://github.com/FelipePrado0/gow3_pc) `9a6d17c..2a0eac6` (105 commits; os ports copiaram `9a6d17c`), **só a parte de núcleo**, renomeada para `TLG_`/`tlg_` e mesclada em 3 vias sobre as nossas mudanças (chave de permutação de shader, transições parciais, `fs:[0]` em qualquer registrador, wave64, stencil, write merge e o resto ficam como estavam). Hashes do upstream (`up`).

**Runtime e loader**
- TSC do jogo = relógio do processo em ns, frequência 1 GHz (`up 2dea61f`): o `rdtsc` cru difere entre núcleos em alguns hosts e o relógio do jogo pulava.
- **Reescrita ISA para CPUs sem MOVBE, BMI1 ou LZCNT** (Sandy/Ivy Bridge, FX antigos; PR #129 do upstream: `up 623e109`, `a09e8e1`, `8af8a8f`, `0601854`, `99ec867`, `1884be4`, `8144011`, `82f825f`, `01a9224`, `a445c6e`, `80dec4c`, `ee2fc0f`, `38637d9`). `tlg-probe --cpu-features` diz `CPU: complete` ou `CPU: missing ...` (exit 20 = falta conjunto sem reescrita, AVX e anteriores; `TLG_CPU_CHECK=0` pula). `scripts/isa_rewrite.py` (iced-x86 1.21.0) grava `out/isa-rewrite.bin` (tabela G3ISA1, presa ao SHA-256 da imagem); o probe com `--isa-rewrite <tabela>` cria stubs depois da imagem (jmp rel32, jmp com a próxima instrução realocada, ou int3 via SIGTRAP), emula em C com flags exatas (`isa_emulate.c`, `-mgeneral-regs-only`) e relata os sítios mais executados ao sair. `TLG_ISA_REWRITE=1` força em qualquer CPU (teste), `=0` nunca. **Cada runner de port precisa** rodar `--cpu-features` depois do `link_modules.py` e, se faltar algo, o `isa_rewrite.py`, e passar `--isa-rewrite` (feito no `infamous_run.py`; `port/run.py` é o exemplo do upstream). Os pacotes instalam e congelam o iced-x86.
- `tlg-probe --gamepad`: uma linha com o tipo e as letras do primeiro controle (SDL3); `--mount guest=host` (até 8) monta um arquivo ou pasta do host por cima do jogo (`up 9155cdc`, só a parte genérica).
- Preparação: eboot/prx/param.sfo vazios dão erro com o nome do arquivo (`up 5dc44e4`); o link registra o hash do arquivo lido (`up 6d580ba`, `e2cd838`); `mods.py` recusa junções do Windows (`up db01ed7`); saída UTF-8 nos scripts (`up f4e4ffe`); testes no Windows (`up 9a1c8f7`, `9855e52`).

**GPU**
- **Linhas pretas na RDNA2 e na Intel (Windows):** os drivers proprietários da AMD e da Intel no Windows leem errado a profundidade de 2 amostras buscada por shader. Nesses drivers as imagens multisample do jogo viram uma amostra (`tlg_msaa.h`, padrão por `VkDriverId` em `tlg_msaa_default.h`); tamanhos e endereços do guest continuam com a contagem real. `TLG_NO_MSAA=1` liga em qualquer GPU, `=0` mantém o multisampling; o log mostra `GPU: guest multisampling on|off`. NVIDIA, RADV, AMDVLK e ANV não mudam (`up 10a7864`, `6b51083`, `55eaa5d`). `tlg-gpu-capabilities --list` ganhou as colunas do padrão e da GPU preferida.
- Variáveis de teste para bisseção (cada uma aparece no log quando ligada): `TLG_LIGHTING_TEST=shading,msdepth,dref,bias|all`, `TLG_AMD_PATHS_OFF=mixed,shaderext|all` (`up 00e4710`).
- Depuração: `TLG_WAVE_STATS=1` registra a largura de wave de cada estágio e, na AMD, ISA e VGPRs (`up 60d03f3`, `edc9b18`, `191854e`, `8adcb78`); `TLG_FULL_BARRIERS=1` põe barreira completa em volta de cada passe e dispatch (`up 268a2fd`, `51861f3`).
- Captura de quadro: censo por passe (amostras, caminho, stencil, clears, layout) (`up dd25c58`); `TLG_DIAG_IMAGES=1` grava as imagens de cada passe/dispatch em `TLG_DIAG_DIR` (`up 55e6603`); crash da captura lendo o endereço zero corrigido (`up 081df6f`, `0c03388`).

**Janela e menu**
- Cursor do sistema escondido sobre o jogo (`up 252a279`); com o mouse do TLG ele volta só enquanto o Esc solta o mouse.
- `TLG_GLYPHS=xbox|nintendo`: o menu do port nomeia os botões como no controle (LB/RB, A/B...), bilíngue (`up 9155cdc`).

**Fora (do God of War III ou só do launcher dele):** patches/cheats por assinatura (`b10c460`, `9cfebd7`), `glyphs.py` (redesenha as texturas de botão do GoW3), launcher `gow3_launcher_win.py` (incluindo `9677e4a`, persistência das configurações com escrita atômica, e a opção de multisampling `7704bf7`/`37d8e5b`: os nossos launchers são por jogo), atalho e arte da Steam, fontes, `game_update.py` (`5f88770`: usamos `scripts/make_game_view.py`), AppImage do Linux (`1da8050`, `1283947`, `00fabd2`: temos `packaging/linux` e `appimage.sh`), README e docs.

## 13/10/2026: snapshot inicial e sincronização dos três ports

**ps4_core**
- Repositório criado a partir da `main` do `infamous_pc` (`4448ef2`), com o `CORE_MANIFEST`, o `tools/core_sync.py` e os submódulos (offline).
- Sincronizado com `infamous_pc` `f76850a` (correção dos bindings do loader e write merge) e com `tlg_pc` `f3eece1` (pós-processamento e mouse); `gr2_pc` `a0fd165` não tinha nada novo. Arquivos divergentes mesclados à mão: `build.sh`, `gpu/CMakeLists.txt`, `tlggpu.cpp`, `tlggpu.h`, `runtime_pad.c`, `vk_rasterizer.cpp`, `vk_scheduler.cpp`, `avplayer_source.cpp`.
- `core_sync.py --status-all [--fetch]`: tabela dos três ports; a linhagem usa o commit fixado do `CORE_SOURCE`; merges contam no histórico (`git log -m`).
- `tools/list_exports.py` gera o `docs/LIBRARIES.md` (bibliotecas, funções reais e stubs, cobertura por jogo).
- `docs/KNOWN_ISSUES.md`, este changelog, README reorganizado; rascunho de `port/src/runtime_game.h` (não ligado).

**Loader**
- `mov reg, fs:[0]` em qualquer registrador (`iss 630be5c`); trace de imports `TLG_IMPORT_TRACE=<n>` para bring-ups.
- Imports resolvidos pelo host não são sobrescritos pelos bindings da libc nativa (`iss 35d2aba`).

**Runtime**
- Pad: sensor de movimento real (SDL3) e simulado, zona morta `TLG_STICK_DEADZONE` (10%), último controle mexido, deslizar no touchpad sem touchpad, avisos na tela (`iss 3c0932f`, `iss 7420110`, `iss c790ccb`, `iss a6bf6d6`).
- Mouse e teclado: o mouse vira o analógico direito (`mouse_stick.h`), botões/roda no `TLG_KEY_MAP`, configurações no menu (`tlg 137e1b1`, `tlg 94de6b6`).
- Orçamento de VRAM padrão de 3500 MB (`iss 3c0932f`).
- Rede/PSN offline, NpToolkit, Font, Json2, Audio3d (GR2, ainda nos arquivos do jogo: `gr2 965c53f`, `gr2 ff54d9f`, `gr2 8cdf58a`).

**GPU**
- Chave das variantes de shader antes da tradução: o FPS não decai (`iss 33378a2`).
- Caminho rápido para falta de leitura; profiler de faltas (`iss 0ed1002`, `iss cf00924`).
- Readback preciso por padrão de novo (`iss 66bf296`); `TLG_WRITE_MERGE=1` e janela de download (`iss 335c0b7`).
- Stencil `test_val`/`op_val` numa referência só (`iss edbcbf9`).
- BC 1D (promoção desligada por padrão, `TLG_BLOCK1D`), transições parciais repetidas puladas, ballots wave64 divergentes, `ReadFirstLane` (`iss e594615`, `iss 5ed8725`, `iss dec86ff`, `iss 4448ef2`).
- Cache de shaders com chave nos fontes do tradutor (`iss dec86ff`).
- SMAA 1x, AMD CAS e ajustes de cor na imagem final (`tlg 4a375f0`, `tlg 7f8fb85`).
- Recorder: esvaziar a fila de draws antes do `EndRendering` (`gr2 58bf2ce`, `tlg 0504e83`).
- AvPlayer: `Stop` acorda as filas e junta as threads (`gr2 929428e`); o log de depuração `tlg-av:` saiu (versão do TLG, `tlg 5ebde2b`).
- Captura de shader com páginas protegidas (`gr2 36a05fb`); diagnóstico de cópia e de compute com lanes (`iss b3d4a90`).
- `TLG_SKIP_ADJACENCY=1` para isolar o passe de contorno com adjacência (`gr2 554c1dd`).

**Empacotamento e janela**
- Nome, app id e ícone da janela por jogo (`gr2 20bb4a1`, `tlg 2278191`); FreeType no build do Windows (`gr2 9fd14de`).

---

<a id="english"></a>

## English

**2026-10-14: sync with infamous_pc `fd78ad5` (PRs #5, #6).** Eager GPU readback (`TLG_EAGER_READBACK=1`, off in the core, on by default in inFAMOUS): at fence packets the GPU copies the buffers the game reads back, and the next read fault is served from that copy without waiting (`c5dd761`); `TLG_READBACK_STATS=1`. Stale-copy fix (`f4c30a1`): CPU writes merged without a fault (`TLG_WRITE_MERGE`) and their upload mark the eager copies of those pages stale (a stale copy used to overwrite the game's new bytes: objects vanished); no eager copy is served after an image readback of the range. Per-game env prefix made generic: `alias_port_environment()` copies `<PREFIX>X` to `TLG_X`, prefix from the port (`PORT_ENV_PREFIX="ISS_"` in `port/packaging/port.env`, passed by `build.sh` as `-DPORT_ENV_PREFIX`; none = no alias) — infamous_pc must add that line to its `port.env`. TSC default back to the host rdtsc (`TLG_TSC=process` opts in; inFAMOUS executes rdtsc itself). `session_chart.py` uncapped mode. Test stubs: `tlggpu_overlay_notice` (pad tests), `runtime_report` (GPU tests). Verified in an infamous_pc `fd78ad5` worktree: `build.sh` and `build.sh --test` pass; `camera-motion-test`/`postfx-test` segfault in `Vulkan::CreateInstance`, same as on inFAMOUS main (pre-existing). The rest of PR #6 is the upstream sync already here; all core-side work (SMAA/CAS, mouse, presenter, `TLG_SKIP_ADJACENCY`) kept.

**2026-10-13: upstream gow3_pc sync (branch `upstream/gow3-sync`).** Core part of FelipePrado0/gow3_pc `9a6d17c..2a0eac6`, renamed to `TLG_`/`tlg_` and 3-way merged over our changes. Runtime: guest TSC = process clock (`2dea61f`); ISA rewrite for CPUs without MOVBE/BMI1/LZCNT (`--cpu-features`, `scripts/isa_rewrite.py` + iced-x86, `--isa-rewrite`, `TLG_ISA_REWRITE`, `TLG_CPU_CHECK`; each port runner must generate and pass the table, done in `infamous_run.py`); `--gamepad`, `--mount guest=host`; preparation diagnostics and file hashes in the link. GPU: guest multisampling off by default on AMD/Intel Windows drivers (RDNA2/Arc black lines; `TLG_NO_MSAA=0|1`); bisect knobs `TLG_LIGHTING_TEST`, `TLG_AMD_PATHS_OFF`; `TLG_WAVE_STATS=1`, `TLG_FULL_BARRIERS=1`; frame capture census, `TLG_DIAG_IMAGES=1`, null-address crash fix. Window: hidden system cursor (shown while Esc releases the TLG mouse); `TLG_GLYPHS=xbox|nintendo` button names in the menu. Not taken: GoW3 patches/cheats, glyphs.py, the GoW3 launcher (incl. its settings-persistence fix), Steam art, game_update.py, the Linux AppImage, docs.

**2026-10-13: initial snapshot, three ports synced.** ps4_core created from infamous_pc main `4448ef2`, then synced with infamous_pc `f76850a` (loader binding fix `35d2aba`, write merge `335c0b7`) and tlg_pc `f3eece1` (SMAA/CAS post-processing, mouse and keyboard); gr2_pc `a0fd165` had nothing new. Diverged files merged by hand (build.sh, gpu/CMakeLists.txt, tlggpu.cpp/.h, runtime_pad.c, vk_rasterizer.cpp, vk_scheduler.cpp, avplayer_source.cpp). New: `core_sync.py --status-all`, pinned lineage, merge-aware history; `tools/list_exports.py` → `docs/LIBRARIES.md`; `docs/KNOWN_ISSUES.md`; this changelog; reorganized README; draft `port/src/runtime_game.h`.

Core fixes of the day (details and hashes above): loader `fs:[0]` in any register and native-libc binding order; pad motion sensor, dead zone, last-used controller, touchpad swipe; mouse and keyboard; VRAM budget; shader permutation key before translation; read-fault fast path; precise readbacks default; write merge; stencil reference merge; BC 1D handling; wave64 ballots and ReadFirstLane; shader cache key; SMAA/CAS; recorder crash; AvPlayer stop; shader capture on protected pages; per-game window identity.
