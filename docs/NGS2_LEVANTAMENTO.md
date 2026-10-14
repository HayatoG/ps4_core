# Levantamento: NGS2 próprio para TLG e GR2

13/10/2026. Base para reimplementar do zero o `libSceNgs2` (mixer de software da Sony), para os ports não precisarem mais do `libSceNgs2.sprx` do firmware (TLG #11 / GR2 #8).

Fontes:
- **Rastreio dinâmico** do NGS2 nativo nos dois jogos (`TLG_NGS2_TRACE`, ver abaixo): GR2 ~4 min e TLG ~5 min, do título até o gameplay. São 4,5 milhões de `VoiceControl` no TLG e 110 mil no GR2.
- **Estudo estático:** TLG em [tlg_pc/analysis/03_audio.md](tlg_pc/analysis/03_audio.md) e GR2 em [gr2_pc/analysis/03_audio.md](gr2_pc/analysis/03_audio.md).
- **Referências gravadas** (ficam fora do git, porque são áudio do jogo): `~/decomp/ngs2_ref/`, com `*_trace*.txt` e `*_ref.wav` (a mixagem final do NGS2 nativo, float 8 canais, 48 kHz).

Legenda: **[C]** visto no rastreio ou no binário; **[D]** deduzido com boa confiança (layout batendo com o SDK público / shadPS4); **[H]** hipótese.

## 1. Conclusão

- **Um NGS2 serve aos dois jogos.** Os dois usam o mesmo middleware "sndx" da Sony por cima do NGS2. Chamam **as mesmas 10 funções** e quase o mesmo conjunto de parâmetros: 40 tipos no total, 36 em comum. [C]
- **A superfície é pequena e fechada:**

  | Funções | Uso |
  |---|---|
  | `SystemCreateWithAllocator` | criar o sistema |
  | `SystemRender` | render |
  | `RackQueryBufferSize`, `RackCreate`, `RackCreateWithAllocator`, `RackGetVoiceHandle` | racks e vozes |
  | `VoiceControl`, `VoiceGetState` (sempre `size` 0x30) | controle das vozes |
  | `SystemDestroy`, `RackDestroy` | destruição; nunca chamadas na sessão |

  Nenhuma chamada retornou erro. [C]
- **Saída:** um buffer float32 LE (tipo 0x18) de 8 canais e 256 frames por render, a 48 kHz. O GR2 pede um **segundo buffer mono** de 256 frames: o alto-falante do controle, vindo da 2ª voz de mastering (`0x30000005`). Portanto o render tem de entregar várias saídas, cada uma com o seu número de canais. [C]
- **Decodificadores:**
  - PCM;
  - **VAG/HEVAG** (tipo 0x1c);
  - **ATRAC9** (tipo 0x40), que no nativo passa pela nossa `runtime_ajm.c`. No nosso NGS2, o ATRAC9 pode chamar o LibAtrac9 direto. [C]
- **Ordem de dificuldade:** sampler + mixer + mastering primeiro. Depois os efeitos: filtro biquad, compressor, reverb I3DL2, EQ e o módulo custom (0x1c / 0x1d).
- Também temos a **saída do nativo gravada**, então cada etapa pode ser comparada número a número.

## 2. Grafo de racks [C]

| Rack ID | Tipo | Opção de criação (size) | Vozes | Observação |
|---|---|---|---|---|
| 0x1000 | SAMPLER | 0xa8 | 256 | uma voz por som tocando |
| 0x2000 | SUBMIXER | 0x98 | 1 por rack | ~40 racks: barramentos, categorias, FX "THRU/EQ/COMP/UMIX…" |
| 0x2001 | REVERB | 0x88 | 1 | I3DL2 |
| 0x3000 | MASTERING | 0x88 | 1 (TLG), 2 (GR2) | saída final |
| 0x4002 | CUSTOM SUBMIXER | 0x4d8 | 1 | módulos custom 0x1c/0x1d |

- **Opção do sistema** (0x40 bytes): `size` u64 em +0, nome, `maxGrainSamples`=512 em +0x1c, `numGrainSamples`=256 em +0x20, `sampleRate`=48000 em +0x24. [C, nomes D]
- **Opção de rack:** `size` u64 em +0 e nome[16]. Os mesmos campos aparecem nos offsets +0x1c (512), +0x20 (`maxVoices`), +0x24, +0x28 e +0x2c (8 = matrizes/canais). [C, nomes D]
- **Ligações vistas** (param PATCH): sampler → submixer; submixer → submixer / reverb / custom / mastering; reverb → submixer; custom → reverb / submixer. É um grafo de barramentos com até 5 portas de saída por voz de sampler (TLG). [C]
  - **[H]** O destino do PATCH dos samplers não bate com os handles de `GetVoiceHandle`. Nos exemplos, o destino é o **handle do rack** (a voz 0 parece ter o mesmo endereço que o rack). A implementação precisa aceitar os dois.

## 3. Parâmetros do `VoiceControl`

Cabeçalho: `u16 size; s16 next; u32 id`. `next` é o deslocamento em bytes até o próximo bloco (0 = fim). Nos jogos, o `next` vem sempre 0: um parâmetro por chamada. [C]

### Genéricos (qualquer rack)

| ID | Nome (SDK) [D] | Layout [C] | TLG | GR2 |
|---|---|---|---|---|
| 1 | MATRIX_LEVELS | `u32 matrix; u32 numLevels; float *levels` (8 ou 48 = 6×8 níveis) | ✔ | ✔ |
| 2 | PORT_VOLUME | `u32 port; float level` | ✔ | ✔ |
| 3 | PORT_MATRIX | `u32 port; u32 matrix` | ✔ | ✔ |
| 5 | PATCH | `u32 port; u32 destInput; void *destHandle` (NULL = desliga) | ✔ | ✔ |
| 6 | KICK_EVENT | `u32 event`: 0 PLAY, 1 RELEASE, 2 STOP, 3 KILL, 4 PAUSE, 5 RESUME | ✔ | ✔ |

### Sampler (0x1000)

| ID | Nome [D] | Layout [C] |
|---|---|---|
| 0x10000000 | SETUP | `u32 waveformType` (0x40 AT9, 0x1c VAG), `u32 channels` (1/2/4/6/8), `u32 sampleRate` (48000), `u32 configData` (AT9), `u32 frameOffset`, `u32 frameMargin`, `u32 flags` |
| 0x10000001 | ADD_WAVEFORM_BLOCKS | `void *data; u32 numBlocks; u32 flags; Block *blocks`; Block (32 B) = `u32 dataOffset, dataSize, numRepeats, numSkipSamples, numSamples, reserved; u64 userData` |
| 0x10000005 | PITCH | `float ratio` (≈0.9–1.0) |
| 0x10000006 | ENVELOPE | `u32 numPoints; u32 numReleasePoints; Point *points`; Point (12 B) = `u32 curve` (0/1/2), `u32 durationSamples`, `float height` |
| 0x10000008 | USER_FX do sampler (só GR2, vozes 3D) | 0x28 B: `handler` + 3 user data, mesmo layout do USER_FX do submixer [D, SDK]: função do sndx (FUN_0125f7c0). A cada render ela lê o PCM mono da voz e chama `sceAudio3dObjectSetAttributes`. Não apareceu nos 4 min rastreados [C estático] |
| 0x10000009 | PEAK_METER (só GR2) | `u32 enable` |
| 0x1000000a | FILTER | `u32 index, location, type, channelMask…; float freq, resonance(0.5), gain(1.0)` |

### Submixer (0x2000)

| ID | Nome [D] | Observação |
|---|---|---|
| 0x20000000 | SETUP | `u32 numChannels` = 8 |
| 0x20000002 | COMPRESSOR | ~8 floats (threshold, ratio, attack, release, gain…) |
| 0x20000004 | USER_FX | **ponteiro para uma função do sndx** e um contexto. UMIX, AMP, DISTO (e PANNER no TLG) são funções do próprio sndx (GR2: 0x1265540 UMIX, 0x1265580 AMP, 0x1265500 DISTO). O NGS2 só precisa **chamá-las** no render, sem implementar esses efeitos [C] |
| 0x20000006 | FILTER | `index, location, type` (1/6/7/8…), `freq` (500/1000/24000), `resonance`, `gain`. Biquad por banda; é o parâmetro mais usado (1,2 M no TLG) |

### Reverb (0x2001)

| ID | Nome [D] | Layout [C] |
|---|---|---|
| 0x20010000 | SETUP | `u32 inChannels` = 8, `u32 outChannels` = 8 |
| 0x20010001 | I3DL2 (96 B) | `float wet, dry; s32 room, roomHF; u32 reflectionPattern; float decayTime, decayHFRatio; s32 reflections; float reflectionsDelay; s32 reverb; float reverbDelay, diffusion, density, hfReference`. O resto é reservado |

### Mastering (0x3000)

| ID | Nome [D] | Valores vistos |
|---|---|---|
| 0x30000000 | SETUP | 8 canais |
| 0x30000003 | LIMITER | enable=1, threshold=1.0 |
| 0x30000004 | GAIN | 1.0 / 1.0 (FBW, LFE) |
| 0x30000005 | OUTPUT (só GR2) | id 1: a 2ª voz de mastering vai para o buffer mono do alto-falante do controle [C] |

### Custom (0x4002)

| ID | Observação |
|---|---|
| 0x40020000 | SETUP do rack custom |
| 0x40001c00 | módulo 0x1c = **CHORUS** (48 B, só TLG) [D] |
| 0x40001d00 | módulo 0x1d = **DELAY** multi-tap (136 B): até 8 pares (nível, tempo em ms: 70, 140, 210, 367…) mais feedback/mix [D] |

## 3b. Dados de som

| | TLG | GR2 |
|---|---|---|
| sndx | `20160930_trico_lto` | `ps4sndx20160617_3500_gravity2_pause_eco_fix` (mais antigo) |
| Bancos | `.bod` + `*_sbuf.data` + `.scs` | 188 `.sxd`, mais 199 pares `.sxd1`/`.sxd2` (streams) |
| ATRAC9 (0x42) | BGM, diálogo, ambiência; até 6 canais | 16.685 ondas, 1/2/4/6/**8** canais, todas a 48 kHz |
| HEVAG (0x21) | quase todo o SFX, inclusive a 96 kHz | 8.023 ondas, 1–6 canais, a 48/44,1/32 kHz e ~200 taxas quebradas, o que exige **resample arbitrário** |
| Outros | PCM raro, 0x20 | 3 ondas dummy 0x28; nenhum PCM nem MP3 |
| FX dos barramentos | THRU, REVERB, COMP, UMIX, DELAY, CHORUS, EQ | EQ 15, COMP 14, REVERB 5, UMIX 3, THRU 2, AMP 2, DISTO 1, DELAY 1 |

- O GR2 usa `enableACP=1`: no nativo, o ATRAC9 passa pelo Ajm. O nosso NGS2 pode ignorar essa flag e usar o LibAtrac9.
- Das 47 exportações do `libSceNgs2.sprx`, **37 não são usadas** por nenhum dos dois jogos.

## 4. Plano de implementação

Arquivo novo `port/src/runtime_ngs2.c`, compartilhado pelos dois ports, ligado por `TLG_NGS2=hle`. O nativo continua sendo o padrão até a comparação ficar boa.

| Etapa | Conteúdo | Validação |
|---|---|---|
| 1 | Objetos sistema/rack/voz, alocador do jogo, `QueryBufferSize`, `GetVoiceHandle`, `VoiceGetState` (0x30), todos os parâmetros aceitos e guardados | o jogo roda igual ao placeholder mudo |
| 2 | Sampler PCM + VAG/HEVAG, blocos/loops, pitch (resample), envelope, eventos; submixer/mastering como soma com volume de porta e matriz | **SFX audível**; comparar com `*_ref.wav` |
| 3 | ATRAC9 via LibAtrac9 (até 8 canais, `configData`) | **BGM e diálogo** |
| 4 | Filtro biquad (sampler e submixer), compressor, limiter, gain | volume e timbre perto do nativo |
| 5 | Reverb I3DL2, chorus (0x1c) e delay (0x1d), USER_FX e callback de voz (chamar funções do sndx) | ambiência; Audio3d do GR2 |
| 6 | Trocar o padrão para `hle`, tirar o `libSceNgs2.sprx` da lista de arquivos necessários | Release sem arquivo de firmware |

Estimativa: etapas 1–3 em 2–3 semanas; 4–5 em mais 2–4.

## 5. Implementação (etapas 1–3, 14/10/2026)

`port/src/runtime_ngs2.c` (branch `feat/ngs2-hle`), sem nada de jogo dentro. Escolha: `TLG_NGS2=native` usa o `.sprx`; qualquer outro valor (o launcher manda `hle` quando o `.sprx` falta ou está desmarcado) usa o mixer próprio. Teste offline: `tests/test_ngs2.c` (`build.sh --test`).

Achados novos [C]:
- **Fim de voz no sndx** (TLG `0x1a85b0`, a função que imprime `VOICE_FINISHED`): depois de `VoiceGetState(h, &st, 0x30)`, a voz é dada como terminada **só quando o bit 0 (INUSE) de `stateFlags` cai**. `(flags & 0x22) == 2` (PLAYING sem EMPTY) marca "tocou"; PLAYING com **EMPTY (0x20)** conta falta de dados (stream atrasado). `numDecodedSamples`/`decodedDataSize` (st+0x10) e `envelopeHeight`/`peakHeight` são copiados para a voz do sndx.
- **Matriz é "output-major"**: `level[out * canais_entrada + in]`. Prova: um exemplo de 48 níveis do GR2 tem 1,0 nos índices 0, 7, 14, 21, 28, 35 (identidade 6→8 com passo 7).
- **Envelope**: cada ponto vai **até** `height` em `duration` amostras (o primeiro parte de 0; `duration` 0 = salto). Os pontos de release partem do nível atual; no fim do release a voz termina.
- **Blocos ATRAC9 de stream**: `skip` 256 só no primeiro bloco; `dataSize` é múltiplo do superframe e `skip + numSamples` fecha superframes inteiros (ex.: 0x9000 B = 48 superframes de 768 B = 49152 amostras = 0x100 + 0xbf00). O stream usa dois buffers de 0x9000 alternados.
- `configData` do ATRAC9 vem como u32 LE `0xfe7a17f0`: os bytes do LibAtrac9 são `fe 7a 17 f0` (ordem invertida).

Estado (etapas 1–3): sampler (PCM, VAG/HEVAG com a tabela de 128 preditores, ATRAC9), blocos (fila, repetições, skip), pitch (Catmull-Rom), envelope, eventos, portas/matrizes/PATCH (rack = voz 0), submixer/reverb/custom/mastering em ordem de dependência, várias saídas (`OUTPUT`), USER_FX (chama a função do jogo, sem a trava do mixer).

### 5.1 Efeitos (etapas 4–5, estudados no `libSceNgs2.sprx`) [C]

O módulo nativo monta cada voz como uma cadeia de "módulos" com parâmetros convertidos e clampados. O que foi lido no binário e reescrito em `runtime_ngs2.c`:

- **Ordem na voz** (sampler e submixer): filtros com `location` 0 → envelope → compressor (submixer) → distorção → USER_FX → filtros `location` 1 → peak meter → para cada porta p: filtros `location` 2+p só no que a porta envia → envio (volume × matriz, **rampa linear no grain** entre o valor anterior e o novo). Início com fade-in de 64 amostras; STOP/KILL com fade-out de 64 amostras no grain seguinte.
- **Filtro** (`0x1000000a`/`0x20000006`, 48 B): `index, location (0..17), type, channelMask` (bits = canais **sem** filtro), `fc, q, level` ou 5 coeficientes diretos (`type 0x20`). Tipos: 0 bypass, 1 LPF, 2 HPF, 3 BPF, 4 BPF 0 dB, 5 notch, 6 low shelf, 7 high shelf, 8 peaking (`level` = ganho linear, A = √level), 9/10 LPF/HPF de 1ª ordem. Projetos RBJ; `fc/fs` limitado a 0,001..0,49987, Q a 1e-4..32.
- **Compressor** (`0x20000002`, 40 B): `flags` (bit0 = bypass, bit1 = detector RMS, bit3 = liga todos os canais menos o LFE), `threshold, ratio, knee, attack, release, level`. "ratio" é a inclinação acima do limiar (saída = thr + (x − thr)·ratio, em amplitude). Detector por blocos de 32 amostras, suavização por amostra com coeficientes `exp(−1/(fs·t))`.
- **Mastering:** LFE passa-baixa de 4ª ordem no canal 3 (Chebyshev 0,1 dB, 120 Hz **ligado por padrão**; 80/120/240 via `0x30000002`), limiter = o mesmo compressor com ratio 0, ataque 1 ms, release 10 ms (`0x30000003` liga e muda o limiar; **não é brickwall**: picos passam alguns % do limiar), ganho (`0x30000004`, rampa).
- **Reverb I3DL2** (`0x20010001`): downmix mono sem LFE (0,267 para 8 canais), linha de reflexões com 4 taps de 8 padrões e um 5º tap (reflectionsDelay + reverbDelay) que alimenta 16 combs (22–43 ms escalados pela densidade, decaimento `10^(−3·L/(fs·decayTime))`, amortecimento por decayHFRatio), cada canal de saída soma 8 combs (máscara própria) e passa por 4 allpasses (11,58/9,19/7,1/4,69 ms com desvios por canal, coeficiente difusão·0,005); saída = reflexões·reflections·room + tardio·reverb·room (o nível do `reverb` tem −24 dB embutidos), passa-baixa de room HF e `wet`/`dry`. O LFE sai como `dry`·entrada. O tamanho do buffer do TLG (0x3e180) indica `reverbSize` 0 (taxa cheia).
- **Custom:** chorus (`0x40001c00`: 1–4 fases em cascata, atraso, LFO senoidal, profundidade, realimentação, dry/wet) e delay multi-tap (`0x40001d00`: dry, wet, ganho, realimentação normalizada pela soma dos taps, passa-baixa de 1 polo, até 8 taps, por canal por padrão). Nos exemplos do TLG o chorus tem wet 0 (só passa o sinal).
- **Envelope:** curvas 0 linear, 1 `(10^x−1)/9`, 2 `log10(1+9x)`, 3 S (cosseno), 4 arco-seno, 5 seno/cosseno de um quarto; um envelope que termina a parte "forward" em 0 encerra a voz.

Validação: `tests/test_ngs2.c` mede resposta dos filtros, curva do compressor, limiter, LFE, ganho, T60 do reverb (1,0 e 2,5 s), tempo do delay e do chorus e as curvas do envelope; custo de um grafo do tamanho do TLG ≈ 1,6 ms por render de 256 amostras (5,3 ms de áudio). `TLG_NGS2_WAV=<arquivo>` grava a mixagem; `tools/ngs2_compare.py a.wav b.wav --align` alinha as duas gravações (a tela de título toca os mesmos streams) e dá diferença de nível e correlação por canal. Com o mixer das etapas 1–3, a tela de título já batia com o nativo: correlação 0,999, −0,4 dB.

## 6. Como repetir o rastreio

```
TLG_NGS2=native TLG_NGS2_TRACE=~/decomp/ngs2_ref/x_trace.txt TLG_NGS2_TRACE_WAV=~/decomp/ngs2_ref/x_ref.wav <rodar o jogo>
```

O código está em `port/src/runtime_ngs2_trace.c`, nas branches `feat/ngs2-hle` dos dois repositórios. Ele embrulha os imports do eboot que foram ligados ao `.sprx` e conta os parâmetros por (rack, id), guardando até 4 exemplos com os dados apontados. Também registra as ligações PATCH e grava a mixagem final.
