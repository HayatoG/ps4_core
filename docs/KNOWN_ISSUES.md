# Problemas conhecidos do núcleo / Core known issues

[English below](#english) · Atualizado em 13/10/2026 · Fonte: `~/decomp/PORT_INFAMOUS.md`, `~/decomp/PADRAO_PORTS.md` e o sync de hoje.

Só o que é do **núcleo** (runtime, loader, GPU). Problemas de um jogo só ficam no `PORT_<SLUG>.md` do port. Os hashes são da `main` do port onde a correção nasceu: `iss` = `infamous_pc`, `gr2` = `gr2_pc` (a linhagem do inFAMOUS contém os commits do GR2), `tlg` = `tlg_pc`. Todos já estão no `ps4_core` (ver `CORE_SOURCE`).

## Em aberto

| # | Problema | Onde | Estado / próximo passo |
|---|---|---|---|
| 1 | **Wave64 em GPUs de 32 lanes (NVIDIA).** O GCN executa ondas de 64; na NVIDIA o tradutor emula com duas metades de 32. Ballots dentro de desvio não uniforme (`lower_wave64_pass: Ballot instruction in non uniform control flow`, 6 shaders no inFAMOUS) ainda podem errar. Suspeito da fumaça/fogo piscando, da grade fina de linhas escuras (iluminação em tiles) e do brilho do dreno que falta. | `shader_recompiler/ir/passes/lower_wave64_pass.cpp` | parcial: ballots divergentes na sua metade (`iss 5ed8725`) e `ReadFirstLane` da primeira lane da onda (`iss 4448ef2`). Falta: ballot em fluxo não uniforme; testar em AMD (wave64 nativo) para separar a causa |
| 2 | **Custo do readback preciso / rastreio por página.** O rastreador de memória trabalha em páginas de 4 KB: cada escrita parcial da CPU numa página modificada pela GPU obriga a baixar a página e esperar a GPU (`DownloadMemory` + `scheduler.Finish()`, 1.600–1.800 vezes a cada 5 s no inFAMOUS). Modo 2 (`TLG_READBACKS=2`, correto): 13–18 FPS; modo 0: ~49 FPS mas sem as partículas na GPU. | `video_core/buffer_cache/`, `page_manager` | paliativos: caminho rápido de falta de leitura (`iss 0ed1002`), `TLG_WRITE_MERGE=1` (desligado por padrão) e janela de download (`iss 335c0b7`). Correção de verdade: **rastrear em pedaços menores que a página** e enviar só as faixas escritas pela CPU, sem baixar (alguns dias) |
| 3 | **Texturas BC 1D.** O Vulkan não aceita formatos BC em imagens 1D (`Bc4/Bc5/Bc1 ... type 1D is not supported`, ~960 avisos). A promoção para 2D existe mas está **desligada** (`TLG_BLOCK1D=1` liga): ligada, o HUD do inFAMOUS ganha listras; desligada, a NVIDIA amostra BC 1D certo apesar de dizer que não suporta. Em drivers que recusam de fato (AMD/Intel?) a textura não é criada. | `texture_cache/`, `vk_instance` | `iss e594615`, `iss 5ed8725`, `iss dec86ff`. Descobrir qual elemento do HUD quebra com a promoção antes de ligar por padrão nos drivers que recusam |
| 4 | **Cache de pipelines em disco derruba na NVIDIA.** `TLG_PIPELINE_BINARIES=1` (VK_KHR_pipeline_binary) cai em `Vulkan::InitializePipelineBinaryCache`. Sem cache em disco, cenas novas compilam 64–118 pipelines por janela de 5 s (engasgo em cutscenes). | `renderer_vulkan/vk_pipeline_cache.cpp` | aberto, prioridade alta. Alternativa: `VkPipelineCache` clássico serializado por `<TITLE_ID>-<chave>` |
| 5 | **Formatos de profundidade sem suporte** (D16S8 e afins) e máscara de stencil. | `vk_instance`, `liverpool_to_vk` | o conflito `test_val`/`op_val` foi resolvido (`iss edbcbf9`); conferir os formatos D16S8 (sombras do inFAMOUS falhando) |
| 6 | **Fila de draws** (`TLG_DRAW_PIPE`): ganho não medido; numa cena do inFAMOUS desligada deu 36 FPS. | `vk_scheduler.cpp` | medir na mesma cena; a correção do recorder (`tlg 0504e83`, `gr2 58bf2ce`) já está |
| 7 | ~~**Hooks do jogo por nome fixo.**~~ **Feito na branch `feat/game-hooks`:** o `runtime.c` passa pela tabela `RuntimeGame` (`port/src/runtime_game.h`), definida pelo port em `src/game_<slug>.c` e escolhida no build (`PORT_GAME` / `PORT_SLUG` / único `game_*.c` / `game_none.c`). | `runtime.c`, `probe.c`, `build.sh` | falta: cada port adotar (`port/games/`); as declarações dos resolvers atuais ficam no `runtime.h` só na transição. [GAME_HOOKS.md](GAME_HOOKS.md) |
| 8 | ~~**Funções de um jogo usadas por outros.**~~ **Feito na branch `feat/game-hooks`:** event flags em `runtime_eventflag.c`; consultas de kernel, fuso horário, `GetModuleInfoForUnwind`, `GetPrtAperture`, `HeapGetTraceInfo`, DiscMap, LoadExec, PlayGo dialog e ErrorDialog em `runtime_system.c`. | ports | cada port aplica `port/games/migration/<port>.patch`. Ainda em arquivo de outro jogo: `sceHttp*` do `runtime_gr2_np.c` usados pelo inFAMOUS |
| 9 | **32 nomes nas tabelas sem NID no `import_names.inc`** (`open`, `read`, `scePadRead`, `sceKernelNanosleep`, `sceRtcGetCurrentClock`…): o `runtime_lookup` só resolve por nome através do `import_names.inc`, então essas entradas não ligam até um jogo trazer a linha. Não afeta os três jogos de hoje. | `import_names.inc` | gerar as linhas quando um jogo novo importar (`scripts/sce_imports.py`); lista em `docs/LIBRARIES.md` |
| 10 | **Padrões do TLG no núcleo.** O mouse do `runtime_pad.c` usa o mapa do TLG (esquerdo = Triângulo, direito = R1, meio = Quadrado). | `runtime_pad.c` | **feito o mecanismo** (`feat/game-hooks`): `mouse_buttons` no `profile.ini` (`TLG_MOUSE_BUTTONS`) troca os botões do mouse por jogo; o mapa embutido continua o do TLG |
| 11 | **Prefixo `TLG_` em todos os ports.** | todo o núcleo | feito o alias: `PORT_ENV_PREFIX` no `port.env` → `probe.c` copia `<PREFIXO>X` para `TLG_X` (inFAMOUS `ISS_`). Depois: prefixo comum `PSCORE_`; plano: `<PREFIXO>_X` com `TLG_X` aceito na transição (`runtime_game.env_prefix`, carregador `core_config.c`) |
| 12 | **Restos do God of War III.** `probe.c` instala cheats e multiplicadores do `CUSA01623 v01.02` (`cheat/damage/actor/orb_hook.h`); `run.sh`/`run.py`/`patches.py` com `CUSA01623` por padrão; `tools/tlg_frame_map.py`. | `probe.c`, scripts | virar `patch_image` de um `game_gow3.c` (ou sair) quando a tabela existir |
| 13 | **Build só do núcleo.** | `build.sh`, `gpu/CMakeLists.txt`, `packaging/` | **feito** (`feat/game-hooks`): `build.sh --test` com `game_none.c`. Falta: empacotamento sem jogo (precisa de `port.env`, ícone e `.rc`); o `tlg_pc` ainda não tem `port/packaging/port.env` |
| 14 | **Sync com o upstream gow3_pc sem teste nos casos que ele resolve.** A reescrita ISA (CPU sem MOVBE/BMI1/LZCNT) e o multisampling desligado por driver (AMD/Intel no Windows) vieram do GoW3; aqui só rodamos em CPU completa e NVIDIA, onde nada muda. Nos outros jogos o padrão sem MSAA pode mudar a imagem nesses drivers. | `isa_rewrite.*`, `tlg_msaa*.h` | testar com `TLG_ISA_REWRITE=1` (força a reescrita) e `TLG_NO_MSAA=1` em cada jogo; um testador com RX 6000 ou Arc no Windows |
| 15 | **TSC por jogo.** Dois relógios: `rdtsc` do host com a frequência medida (padrão) ou o relógio do processo em ns a 1 GHz (`TLG_TSC=process`). Um jogo que executa `rdtsc` **direto** (inFAMOUS 391, TLG 642, GR2 453 sítios) tem de usar o do host, senão roda 3–4x rápido. | `runtime_kernel.c`, `runtime_profile.c`, `probe.c` | **feito** (`feat/game-hooks`): `tsc = host|process|auto` no perfil; `auto` conta `0F 31` no código do eboot ao carregar e escolhe `host` com ≥ 64 sítios (`tsc_auto_threshold`), com log. Falta medir num jogo que só usa as chamadas do kernel (GoW3) |
| 16 | **Readback antecipado + write merge só no inFAMOUS.** `TLG_EAGER_READBACK=1` e `TLG_WRITE_MERGE=1` (desligados no núcleo) ficam ligados por padrão só no launcher do inFAMOUS; a correção das cópias velhas (`iss f4c30a1`: escrita sem falta marca a cópia antecipada como velha; nada é servido depois de um readback de imagem) só foi testada lá. | `buffer_cache.*`, `vk_rasterizer.cpp` | medir e testar no TLG e no GR2 antes de ligar por padrão no núcleo (perfil do jogo) |

## Corrigido hoje (13/10/2026)

| Correção | Commit |
|---|---|
| Loader: `mov reg, fs:[0]` em qualquer registrador vira `gs` (o inFAMOUS carrega o ponteiro da thread em rcx/rbx/r14…; o jogo lia o TLS da glibc) | `iss 630be5c` (`tlg 94f6467`) |
| Loader: imports resolvidos pelo host não são mais sobrescritos pelos bindings da libc nativa (`if (resolved) {…} else if (native_libc …)`) | `iss 35d2aba` |
| Variantes de shader com a chave tirada do runtime info **antes** da tradução: o FPS não cai mais de 60 para 6 (um shader chegava a 32.768 variantes) | `iss 33378a2` (`tlg 436eb01`) |
| Falta de leitura sem dados da GPU pula a ida à thread da GPU; profiler de faltas | `iss 0ed1002`, `iss cf00924` |
| Stencil: uma referência Vulkan só para `test_val` e `op_val` quando as máscaras permitem | `iss edbcbf9` |
| BC 1D: imagem 2D de altura 1 só no host, 1D no cache; promoção desligada por padrão (`TLG_BLOCK1D`) | `iss e594615`, `iss 5ed8725`, `iss dec86ff` (`tlg 4e029a6`) |
| Wave64: ballots divergentes na sua metade; `ReadFirstLane` da primeira lane da onda | `iss 5ed8725`, `iss 4448ef2` |
| Cache de shaders com chave = hash dos fontes do tradutor (`TLG_SHADER_CACHE_KEY`) | `iss dec86ff` |
| Transições parciais de imagem repetidas são puladas | `iss 5ed8725` |
| Readback preciso de novo por padrão (partículas na GPU precisam) | `iss 66bf296` |
| `TLG_WRITE_MERGE=1`: `memcpy`/`memset` do jogo em memória modificada pela GPU sem falta nem espera (desligado por padrão); tamanho da janela de download | `iss 335c0b7` |
| Pad: sensor de movimento (giroscópio real pela SDL3, simulado em qualquer controle), zona morta de 10%, último controle mexido, touchpad sem touchpad | `iss 3c0932f`, `iss 7420110`, `iss c790ccb`, `iss a6bf6d6` (`tlg fa9ba6b`) |
| Mouse e teclado: mouse vira o analógico direito, botões e roda mapeáveis (`TLG_KEY_MAP`) | `tlg 137e1b1`, `tlg 94de6b6` |
| Pós-processamento na imagem final: SMAA 1x, AMD CAS, ajustes de cor | `tlg 4a375f0`, `tlg 7f8fb85` |
| Recorder: esvaziar a fila de draws antes do `EndRendering` (crash do driver NVIDIA na intro do TLG e no GR2) | `gr2 58bf2ce` (`tlg 183ab8d`), `tlg 0504e83` |
| AvPlayer: parar todas as threads e acordar as filas antes do join (GR2 travava no `sceAvPlayerStop`) | `gr2 929428e` (`tlg 5ebde2b`) |
| Captura de shader se recupera de páginas protegidas | `gr2 36a05fb` |
| Nome, app id e ícone da janela por jogo | `gr2 20bb4a1` (`tlg 2278191`) |
| Diagnóstico: avisos da camada de cópia com o caminho; uma linha por compute shader largo com operações de lane | `iss b3d4a90` |

---

<a id="english"></a>

## English

Core-only problems (runtime, loader, GPU); game-only problems stay in the port's `PORT_<SLUG>.md`. Hashes: `iss` = infamous_pc, `gr2` = gr2_pc, `tlg` = tlg_pc; all are in ps4_core.

**Open:**
1. **Wave64 on 32-lane GPUs (NVIDIA):** ballots in non-uniform control flow are still emulated wrong (6 inFAMOUS shaders); suspected for flickering smoke/fire, the thin dark grid and the missing drain glow. Partly fixed (`iss 5ed8725`, `iss 4448ef2`); test on AMD to separate the cause.
2. **Precise readback cost / page-granular tracking:** every partial CPU write into a GPU-modified 4 KB page downloads the page and waits for the GPU (1,600–1,800 times per 5 s in inFAMOUS; mode 2 13–18 FPS vs ~49 in mode 0). Workarounds: read-fault fast path (`iss 0ed1002`), `TLG_WRITE_MERGE=1` and the download window (`iss 335c0b7`). Real fix: sub-page tracking, upload only CPU-written ranges.
3. **BC 1D textures:** Vulkan has no BC 1D; 2D promotion exists but is off (`TLG_BLOCK1D=1`) because it stripes the inFAMOUS HUD; NVIDIA samples BC 1D fine despite reporting no support. Drivers that really refuse need the promotion fixed first.
4. **Pipeline binaries crash on NVIDIA:** `TLG_PIPELINE_BINARIES=1` dies in `InitializePipelineBinaryCache`; without a disk pipeline cache, new scenes stutter (64–118 compiles per 5 s). Try a classic serialized `VkPipelineCache`.
5. Unsupported depth formats (D16S8); stencil `test_val`/`op_val` already fixed (`iss edbcbf9`).
6. Draw pipe (`TLG_DRAW_PIPE`) gain not measured.
7. ~~Game hooks called by fixed name~~ — done on `feat/game-hooks`: `RuntimeGame` table (`port/src/runtime_game.h`) in the port's `src/game_<slug>.c`, chosen at build time; ports still have to adopt it (`port/games/`, [GAME_HOOKS.md](GAME_HOOKS.md)).
8. ~~Functions in one game's file used by all three~~ — done on `feat/game-hooks`: `runtime_eventflag.c` and `runtime_system.c`; ports apply `port/games/migration/<port>.patch`. Still in another game's file: GR2's `sceHttp*` used by inFAMOUS.
9. 32 table names have no NID line in `import_names.inc` and cannot resolve yet (no current game imports them).
10. TLG mouse defaults (Triangle/R1/Square) in `runtime_pad.c`: per-game override done (`mouse_buttons` in `profile.ini`, `TLG_MOUSE_BUTTONS`).
11. `TLG_` env prefix everywhere: per-game alias done (`PORT_ENV_PREFIX` in `port.env`, `ISS_` in inFAMOUS); common `PSCORE_` later.
12. God of War III leftovers in `probe.c` and run scripts.
13. Core-only build done (`build.sh --test` with `game_none.c`); packaging still needs a game; `tlg_pc` has no `port/packaging/port.env`.
14. Upstream gow3_pc sync (ISA rewrite, no-MSAA per driver) is untested where it matters (CPUs without MOVBE/BMI1/LZCNT, AMD/Intel Windows drivers); try `TLG_ISA_REWRITE=1` and `TLG_NO_MSAA=1` per game.
15. **Per-game TSC:** host rdtsc with measured frequency (default) vs process clock at 1 GHz (`TLG_TSC=process`). Games executing rdtsc directly (inFAMOUS: 391 sites) bypass `sceKernelReadTsc` and must use the host rdtsc, or they run 3-4x fast. Done on `feat/game-hooks`: profile `tsc = host|process|auto`; `auto` counts `0F 31` in the eboot's code at load (≥ 64 sites → host, logged). TLG 642, GR2 453, inFAMOUS 391 → host.
16. **Eager readback + write merge on by default only in inFAMOUS** (off in the core); the stale-copy fix (`iss f4c30a1`) is tested only there. Measure on TLG and GR2 before enabling elsewhere.

**Fixed today:** see the table above (loader `fs:[0]` any register and native-libc binding order, shader permutation key before translation, read-fault fast path, stencil reference merge, BC 1D handling, wave64 ballots/ReadFirstLane, shader cache key, partial transitions, precise readbacks default, write merge, pad motion/dead zone/touchpad, mouse and keyboard, SMAA/CAS post-processing, recorder crash, AvPlayer stop, shader capture on protected pages, per-game window identity, diagnostics).
