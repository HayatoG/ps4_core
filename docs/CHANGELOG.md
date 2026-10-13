# Changelog do núcleo / Core changelog

[English below](#english). Hashes da `main` do port onde a mudança nasceu (`iss` = infamous_pc, `gr2` = gr2_pc, `tlg` = tlg_pc). O `ps4_core` só junta: os commits daqui dizem de qual snapshot cada arquivo veio (ver também `CORE_SOURCE`).

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

**2026-10-13: initial snapshot, three ports synced.** ps4_core created from infamous_pc main `4448ef2`, then synced with infamous_pc `f76850a` (loader binding fix `35d2aba`, write merge `335c0b7`) and tlg_pc `f3eece1` (SMAA/CAS post-processing, mouse and keyboard); gr2_pc `a0fd165` had nothing new. Diverged files merged by hand (build.sh, gpu/CMakeLists.txt, tlggpu.cpp/.h, runtime_pad.c, vk_rasterizer.cpp, vk_scheduler.cpp, avplayer_source.cpp). New: `core_sync.py --status-all`, pinned lineage, merge-aware history; `tools/list_exports.py` → `docs/LIBRARIES.md`; `docs/KNOWN_ISSUES.md`; this changelog; reorganized README; draft `port/src/runtime_game.h`.

Core fixes of the day (details and hashes above): loader `fs:[0]` in any register and native-libc binding order; pad motion sensor, dead zone, last-used controller, touchpad swipe; mouse and keyboard; VRAM budget; shader permutation key before translation; read-fault fast path; precise readbacks default; write merge; stencil reference merge; BC 1D handling; wave64 ballots and ReadFirstLane; shader cache key; SMAA/CAS; recorder crash; AvPlayer stop; shader capture on protected pages; per-game window identity.
