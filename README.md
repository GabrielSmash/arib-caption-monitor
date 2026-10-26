# ARIB Caption Monitor

Monitor desktop (Linux, Qt6) de legendas ARIB no padrão brasileiro (ABNT NBR 15606-1 / ISDB-Tb)
recebidas em MPEG-TS por UDP.

- Indica se os pacotes estão chegando (pkt/s, bitrate, origem, tempo desde o último datagrama).
- Decodifica as legendas em tempo real com a libaribcaption (`kABNT_NBR_15606_1_Latin`), mantendo a
  tela como um receptor: `CS` limpa, statements sem `CS` se sobrepõem, posições/cores/tamanhos do emissor.
- Painel lateral: PAT, PMT (com descritores), SDT, PIDs (pacotes, bitrate, erros de continuidade),
  gerenciamento da legenda (idioma, DMF, formato, TCS, rollup_mode), formato de exibição (SWF, SDF,
  SDP, SSM, SHS/SVS, SCR) e as regiões na tela.
- Aba "Comandos ARIB": cada data group recebido com seus comandos de controle (CS, APR, SDF, SDP, cores…).
- "Mostrar posicionamento": desenha o plano, a área SDF/SDP (amarelo) e as regiões (ciano).
- Aba "Validação": 29 regras automáticas (PMT, PES/data group com CRC-16, gerenciamento, conjunto de
  caracteres Latin, códigos de controle, layout em relação à área SDF/SDP e ao plano, acessibilidade).
  Cada regra mostra a **fonte** — sem o texto da ABNT NBR 15606-1, regras da operação japonesa
  (ARIB TR-B14) são **avisos**, não erros. Severidade, contagem de falhas/verificações e últimas ocorrências.

## Dependências

```bash
sudo apt install build-essential cmake pkg-config qt6-base-dev libfreetype-dev libfontconfig-dev fonts-dejavu
```

A libaribcaption **não** precisa estar instalada: o CMake baixa uma versão fixa do upstream, aplica
`patches/libaribcaption-deferred-wrap.patch` e compila estática junto com o app (precisa de `git` e internet
no primeiro `cmake`). Detalhes em `patches/README.md`.

## Build

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
ctest --test-dir build        # layout (quebra de linha) e validação, com vetores reais (Globo, SkylarBox)
```

## Uso

```bash
./build/arib-caption-monitor                  # porta salva (padrão 1526), unicast
./build/arib-caption-monitor -p 1526 -m 239.1.1.1   # multicast
```

O stream pode ser UDP puro ou RTP (detectado automaticamente). O PID da legenda é escolhido pela PMT
(stream_type 0x06 com component_tag 0x30–0x37 ou data_component_id 0x0008).

Para testes automatizados sem tela: `QT_QPA_PLATFORM=offscreen ./build/arib-caption-monitor --tab 1 --snapshot shot.png --snapshot-after 10`
(`--tab`: 0 MPEG-TS, 1 Validação, 2 Comandos, 3 Log).

## Limitações conhecidas

- Quebra de linha na borda da área é **adiada** até o próximo caractere (patch na libaribcaption), como nos
  receptores reais; a libaribcaption original quebraria imediatamente.
- `SCR` (scroll) é exibido no painel, mas a libaribcaption ignora o comando ao desenhar.
- As legendas são exibidas na chegada (monitor ao vivo); o PTS é mostrado, mas não usado para atrasar a exibição.
