# Changelog do núcleo / Core changelog

[English below](#english). Hashes da `main` do port onde a mudança nasceu (`iss` = infamous_pc, `gr2` = gr2_pc, `tlg` = tlg_pc). O `ps4_core` só junta: os commits daqui dizem de qual snapshot cada arquivo veio (ver também `CORE_SOURCE`).

## 14/10/2026: NGS2 embutido (branch `feat/ngs2-hle`, em cima de `sync/2026-10-14`)

Do `tlg_pc` `feat/ngs2-hle` (`1d116a7`, `c9d23b0`, `74a891b`, `bd122ed`; ainda não está na `main` do TLG). A mesma coisa já roda no GR2 na branch de teste `gr2_pc` `test/gr2-ngs2-avstop-sse4a` (`dc84b9e`, `runtime_ngs2.c` idêntico). Fica numa branch própria para a `sync/2026-10-14` poder entrar sozinha.

**Runtime**
- **`libSceNgs2` do zero** (`port/src/runtime_ngs2.c`), as 10 funções que o middleware sndx da Sony usa no TLG e no GR2: sistema/rack/voz, sampler (todos os PCM, VAG/HEVAG, ATRAC9 pelo LibAtrac9), fila de blocos, reamostragem Catmull-Rom, envelope, grafo de PATCH com matrizes e mastering, várias saídas (o alto-falante do controle do GR2), USER_FX do jogo, e os efeitos (filtros, compressor, limitador, LFE, ganho, reverb I3DL2, chorus, delay) (`tlg 74a891b`, `bd122ed`). Usado quando `TLG_NGS2` não é `native`; `TLG_NGS2=native` deixa o `libSceNgs2.sprx` do firmware (ligado pelo runner do jogo) responder. O `runtime.c` consulta `runtime_ngs2_resolve` antes dos resolvers dos jogos, então os marcadores silenciosos do `runtime_tlg.c` de um port antigo ficam sem uso (com `native` os dois devolvem 0 e o módulo nativo vale).
- **Rastreio do NGS2 nativo** (`port/src/runtime_ngs2_trace.c`, `tlg 1d116a7`): `TLG_NGS2_TRACE` embrulha os imports ligados ao `libSceNgs2.sprx` (`probe.c` chama `runtime_ngs2_trace_bind` para cada bind nativo de função) e conta parâmetros, grafo e layouts; `TLG_NGS2_TRACE_WAV` grava a mixagem nativa.
- Diagnóstico do mixer embutido: `TLG_NGS2_WAV` grava a mixagem; `TLG_NGS2_REVERB`, `TLG_NGS2_CUSTOM`, `TLG_NGS2_DELAY_TYPE` para comparar efeitos; `port/tools/ngs2_compare.py` compara nível, atividade e centróide por canal com o `*_ref.wav` nativo.

**Testes e docs**
- `tests/test_ngs2.c` no `build.sh --test` (`ngs2-test`). Conferido num worktree do `infamous_pc` `2b82d72` com este núcleo: `build.sh` e `build.sh --test` passam (`ngs2-test: all passed`).
- `docs/NGS2_LEVANTAMENTO.md` (levantamento do TLG e do GR2, `tlg c9d23b0`); `docs/LIBRARIES.md` regenerado (`libSceNgs2`: 10 funções reais).
- Fica no jogo: launcher (status "mixer embutido" quando falta o módulo), `tlg_run.py`/`gr2_run.py` (`TLG_NGS2=hle` sem o módulo), README do jogador, e a retirada dos marcadores NGS2 do `runtime_tlg.c`.

## 14/10/2026: sync com o `infamous_pc` `2b82d72` (PRs #13–#15), `gr2_pc` e `feat/tlou-imports` (branch `sync/2026-10-14`)

Do inFAMOUS desde `fd78ad5`, mesclado em 3 vias com base `iss fd78ad5` (o lado do núcleo — SMAA/CAS, mouse, presenter, `TLG_SKIP_ADJACENCY`, upstream — ficou inteiro); do GR2, duas branches que ainda não estão na `main` dele; e as três correções de núcleo da `feat/tlou-imports`. O `tlg_pc` (`c8f249b`, PR #18) e o `gr2_pc` (`40d0391`, PR #15) não têm mais nada: a reserva da janela do sistema no Windows é a mesma da `fix/windows-system-window` daqui, e a dica de VRAM é do launcher de cada jogo.

**Runtime e loader**
- **Windows: janela reservada do sistema** `0x700000000–0x800000000` reservada antes de qualquer DLL (branch `fix/windows-system-window`, PR #16; `iss 1dc82ca`, `tlg ca9a103`, `gr2 57b7013`).
- **Reescrita ISA emula SSE4A** `EXTRQ`/`INSERTQ` (`iss 3727fde`): o Jaguar do PS4 tem SSE4A e nenhuma CPU Intel tem (o eboot do GR2 1.11 tem 96 `EXTRQ`). `TLG_ISA_REWRITE` aceita uma lista de conjuntos (`sse4a`, `movbe,bmi1`) além de `0`/`1`. `isa-sse4a-test` (na AMD a CPU é o oráculo; fora dela, resultados gravados de uma AMD) e `cpu-check-test` no `build.sh --test` e como alvos do Windows.
- **O prefixo do jogo ganha do `TLG_`** (`gr2 5c5d84d`): `alias_port_environment()` copia `<PREFIXO>X` para `TLG_X` mesmo com `TLG_X` definido, como pede o AGENTS.md (o nome mais específico ganha): runners e scripts põem padrões em `TLG_*` que o `GR2_X`/`ISS_X` do jogador tem de trocar. O `port/run.py` lê `<PORT_ENV_PREFIX>ISA_REWRITE` (do ambiente ou do `packaging/port.env`) em vez de um `ISS_ISA_REWRITE` fixo.
- **Memória direta de 4096 MiB** aceita (`TLG_DMEM_MB` 4096..16384; o TLOU precisa dos 4608 de um PS4 comum) e **hooks de memória da GPU** de faixas vizinhas do mesmo tipo se juntam, fila de 1024 (o TLOU desmapeia centenas de mapeamentos de 1 MiB numa chamada: `STOP: GPU hook queue overflow`) (`feat/tlou-imports` `02be2e2`, `6691698`).
- **AvPlayer: `Stop` não trava** (`gr2 c5b9770`, branch `fix/avplayer-stop-join`): `Kernel::Thread::Join` espera a flag `finished` e solta a thread (`detach`). O GR2 ficava 86 s no `sceAvPlayerStop` num `join` de um decodificador cuja função já tinha voltado.

**GPU** (PR #14, correções do fork [shadPS4-Infamous-FixBuild](https://github.com/rayhlancaner-png/shadPS4-Infamous-FixBuild), créditos no `gpu/VENDOR.txt`)
- Atomics de float min/max com um atomic inteiro só (`iss 6110de8`); stencil com os valores de teste e de troca juntos quando dá (`iss f00cd54`).
- VRAM: alocar além do orçamento em vez de falhar (`iss efbff18`), memória local paginável (`VK_EXT_pageable_device_local_memory`, `iss 6c5051b`), residência em grãos de 16 MB tirados de blocos de 256 MB (`iss 662a787`), nova tentativa liberando texturas velhas quando a residência fica sem memória (`iss b1955e1`), uploads de gigabytes em partes de 256 MB (`iss 49b2f42`).
- **Pipelines de compute fora da thread da GPU** (`iss 45d18d1`, ligado por padrão; `TLG_COMPUTE_ASYNC_BUILD=0` volta a montar na hora): cada um levava 60–190 ms no inFAMOUS e um efeito traz dezenas (travadas de 0,6–3,8 s). Nova tentativa quando o driver falha ao criar um pipeline (`iss 5aaae80`).
- **Envio em pedaços** (`iss c4b0956`, ligado por padrão): a cada ~1024 draws/dispatches gravados (`TLG_SUBMIT_DRAWS`, `0` desliga) o trabalho vai para a GPU onde o próximo draw abriria um render pass.
- **Readback adiado** (`iss 63a2197`, `TLG_DEFERRED_READBACK=1`, só com readback preciso; desligado no núcleo, o launcher do inFAMOUS liga): a thread do jogo espera só as suas próprias cópias. Bit `DeferredReadback` no `TLG_TOGGLE_FILE` para A/B.
- **Labels de fence depois das cópias antecipadas** marcam a página como velha (`feat/tlou-imports` `1ade586`): `EventWriteEop`/`EventWriteEos`/`ReleaseMem` copiavam e só depois escreviam o label, e a leitura servida pela cópia não o via (TLOU: `HALT` no `WaitOnContextDone`).
- Captura de shader repetida na thread da GPU parte de um `Info` novo (`feat/tlou-imports` `02be2e2`; antes, a assertiva do `GenerateSrtProgram`).

**Diagnóstico**
- **Gravador de quadro** (`iss 420ab44`): `TLG_FRAME_RECORD_TRIGGER=<arquivo>` grava dois quadros comando por comando em `TLG_DUMP_DIR` (`TLG_FRAME_RECORD_GPU_MB`, padrão 256); `scripts/frame_diff.py` compara os dois (entrou no `CORE_MANIFEST`).
- `TLG_GPU_TIMESTAMP=fixed` avança os timestamps da GPU um passo fixo por escrita (`TLG_GPU_TIMESTAMP_STEP_US`) (`iss e5f8d0c`); `TLG_FRAME_DUMP_COUNT=N` grava N quadros seguidos por gatilho (`iss 69aa859`).

**Outros**
- `.gitignore`: `/dump`, `/game` e `port/user` também como symlinks (teste de boot num worktree, `gr2 5c5d84d`).
- Fica no jogo: padrões do launcher (readback adiado ligado no inFAMOUS, dica de VRAM, layout), `infamous_run.*`, `gr2_run.py`, README, CI.

**Testes**
- Conferido num worktree do `infamous_pc` `2b82d72` com este núcleo (e `PORT_ENV_PREFIX="ISS_"` no `port.env`): `build.sh` e `build.sh --test` passam (incluindo `sema-test`, `isa-sse4a-test` com a CPU como oráculo e `cpu-check-test`); `test_run_isa`, `test_isa_rewrite` e `test_probe` passam. Os 7 erros do `unittest discover` são de arquivos que só o TLG ou o GoW3 têm (`tlg_launcher_win`, `tlg_assets`, `patches/God_of_War_III_Remastered.xml`).

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

**2026-10-14: built-in NGS2 (branch `feat/ngs2-hle`, on top of `sync/2026-10-14`).** From tlg_pc `feat/ngs2-hle` (`1d116a7`, `c9d23b0`, `74a891b`, `bd122ed`; not on TLG main yet; the same `runtime_ngs2.c` runs on gr2_pc `test/gr2-ngs2-avstop-sse4a`). `port/src/runtime_ngs2.c`: a from-scratch libSceNgs2 for the 10 functions the sndx middleware uses (PCM/VAG/ATRAC9 sampler, resampling, envelope, PATCH graph, mastering, multiple outputs, user FX, filters/compressor/limiter/LFE/gain/I3DL2/chorus/delay), used unless `TLG_NGS2=native`; resolved before the games' resolvers. `runtime_ngs2_trace.c`: `TLG_NGS2_TRACE`/`TLG_NGS2_TRACE_WAV` wrap the native module's imports (`probe.c` `runtime_ngs2_trace_bind`). `TLG_NGS2_WAV`, `TLG_NGS2_REVERB`, `TLG_NGS2_CUSTOM`, `TLG_NGS2_DELAY_TYPE`, `port/tools/ngs2_compare.py`. `ngs2-test` in `build.sh --test` (passes in an infamous_pc `2b82d72` worktree with this core). `docs/NGS2_LEVANTAMENTO.md`, regenerated `docs/LIBRARIES.md`. Launcher/runner/README changes and dropping the placeholders from `runtime_tlg.c` stay in the games.

**2026-10-14: sync with infamous_pc `2b82d72` (PRs #13–#15), gr2_pc branches and `feat/tlou-imports` (branch `sync/2026-10-14`).** 3-way merged on base `iss fd78ad5`, core side kept. Windows: the system window `0x700000000–0x800000000` reserved before any DLL (PR #16 `fix/windows-system-window`; same as tlg_pc PR #18 and gr2_pc PR #15, which bring nothing else). ISA rewrite emulates SSE4A `EXTRQ`/`INSERTQ` (every Intel CPU; `iss 3727fde`), `TLG_ISA_REWRITE` takes a set list; `isa-sse4a-test`, `cpu-check-test`. The game's env prefix now wins over a `TLG_X` that is also set (`gr2 5c5d84d`; AGENTS.md: the more specific name wins); `port/run.py` reads `<PORT_ENV_PREFIX>ISA_REWRITE` instead of a fixed `ISS_`. AvPlayer `Stop` no longer hangs: `Kernel::Thread::Join` waits on `finished`, then detaches (`gr2 c5b9770`, GR2 86 s in `sceAvPlayerStop`). From the shadPS4-Infamous-FixBuild fork (PR #14): float atomic min/max with one integer atomic, stencil test+replace values, allocate past the VRAM budget, pageable device local memory, 16 MB residency granules, residency OOM retry, 256 MB staging parts, compute pipelines built off the GPU thread (on; `TLG_COMPUTE_ASYNC_BUILD=0`), pipeline creation retry, submit every ~1024 recorded draws (on; `TLG_SUBMIT_DRAWS`, `0` off), deferred readback (`TLG_DEFERRED_READBACK=1`, precise mode only, off in the core). Frame recorder `TLG_FRAME_RECORD_TRIGGER` + `scripts/frame_diff.py`; `TLG_GPU_TIMESTAMP=fixed`; `TLG_FRAME_DUMP_COUNT=N`. From `feat/tlou-imports`: `TLG_DMEM_MB` down to 4096, merged GPU memory hooks with a 1024 queue, fence labels mark eager copies stale, shader capture retry from a fresh Info. `.gitignore` also covers symlinked `/dump`, `/game`, `port/user`. Verified in an infamous_pc `2b82d72` worktree with this core: `build.sh` and `build.sh --test` pass (sema-test included); the 7 `unittest discover` errors are TLG/GoW3-only files.

**2026-10-14: sync with infamous_pc `fd78ad5` (PRs #5, #6).** Eager GPU readback (`TLG_EAGER_READBACK=1`, off in the core, on by default in inFAMOUS): at fence packets the GPU copies the buffers the game reads back, and the next read fault is served from that copy without waiting (`c5dd761`); `TLG_READBACK_STATS=1`. Stale-copy fix (`f4c30a1`): CPU writes merged without a fault (`TLG_WRITE_MERGE`) and their upload mark the eager copies of those pages stale (a stale copy used to overwrite the game's new bytes: objects vanished); no eager copy is served after an image readback of the range. Per-game env prefix made generic: `alias_port_environment()` copies `<PREFIX>X` to `TLG_X`, prefix from the port (`PORT_ENV_PREFIX="ISS_"` in `port/packaging/port.env`, passed by `build.sh` as `-DPORT_ENV_PREFIX`; none = no alias) — infamous_pc must add that line to its `port.env`. TSC default back to the host rdtsc (`TLG_TSC=process` opts in; inFAMOUS executes rdtsc itself). `session_chart.py` uncapped mode. Test stubs: `tlggpu_overlay_notice` (pad tests), `runtime_report` (GPU tests). Verified in an infamous_pc `fd78ad5` worktree: `build.sh` and `build.sh --test` pass; `camera-motion-test`/`postfx-test` segfault in `Vulkan::CreateInstance`, same as on inFAMOUS main (pre-existing). The rest of PR #6 is the upstream sync already here; all core-side work (SMAA/CAS, mouse, presenter, `TLG_SKIP_ADJACENCY`) kept.

**2026-10-13: upstream gow3_pc sync (branch `upstream/gow3-sync`).** Core part of FelipePrado0/gow3_pc `9a6d17c..2a0eac6`, renamed to `TLG_`/`tlg_` and 3-way merged over our changes. Runtime: guest TSC = process clock (`2dea61f`); ISA rewrite for CPUs without MOVBE/BMI1/LZCNT (`--cpu-features`, `scripts/isa_rewrite.py` + iced-x86, `--isa-rewrite`, `TLG_ISA_REWRITE`, `TLG_CPU_CHECK`; each port runner must generate and pass the table, done in `infamous_run.py`); `--gamepad`, `--mount guest=host`; preparation diagnostics and file hashes in the link. GPU: guest multisampling off by default on AMD/Intel Windows drivers (RDNA2/Arc black lines; `TLG_NO_MSAA=0|1`); bisect knobs `TLG_LIGHTING_TEST`, `TLG_AMD_PATHS_OFF`; `TLG_WAVE_STATS=1`, `TLG_FULL_BARRIERS=1`; frame capture census, `TLG_DIAG_IMAGES=1`, null-address crash fix. Window: hidden system cursor (shown while Esc releases the TLG mouse); `TLG_GLYPHS=xbox|nintendo` button names in the menu. Not taken: GoW3 patches/cheats, glyphs.py, the GoW3 launcher (incl. its settings-persistence fix), Steam art, game_update.py, the Linux AppImage, docs.

**2026-10-13: initial snapshot, three ports synced.** ps4_core created from infamous_pc main `4448ef2`, then synced with infamous_pc `f76850a` (loader binding fix `35d2aba`, write merge `335c0b7`) and tlg_pc `f3eece1` (SMAA/CAS post-processing, mouse and keyboard); gr2_pc `a0fd165` had nothing new. Diverged files merged by hand (build.sh, gpu/CMakeLists.txt, tlggpu.cpp/.h, runtime_pad.c, vk_rasterizer.cpp, vk_scheduler.cpp, avplayer_source.cpp). New: `core_sync.py --status-all`, pinned lineage, merge-aware history; `tools/list_exports.py` → `docs/LIBRARIES.md`; `docs/KNOWN_ISSUES.md`; this changelog; reorganized README; draft `port/src/runtime_game.h`.

Core fixes of the day (details and hashes above): loader `fs:[0]` in any register and native-libc binding order; pad motion sensor, dead zone, last-used controller, touchpad swipe; mouse and keyboard; VRAM budget; shader permutation key before translation; read-fault fast path; precise readbacks default; write merge; stencil reference merge; BC 1D handling; wave64 ballots and ReadFirstLane; shader cache key; SMAA/CAS; recorder crash; AvPlayer stop; shader capture on protected pages; per-game window identity.
