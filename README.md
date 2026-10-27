# ARIB Caption Monitor

Monitor desktop (Linux, Qt6) de legendas ARIB no padrão brasileiro (ABNT NBR 15606-1 / ISDB-Tb)
recebidas em MPEG-TS por UDP ou lidas de arquivos `.ts`, para testar as legendas antes de enviá-las ao
sistema de transmissão.

> **Versão atual: 1.0.0** — a primeira versão instalável em qualquer distribuição Linux.
>
> ⬇️ **[Baixar arib-caption-monitor-1.0.0.flatpak](https://github.com/GabrielSmash/arib-caption-monitor/releases/download/v1.0.0/arib-caption-monitor-1.0.0.flatpak)**
> · [notas da versão](https://github.com/GabrielSmash/arib-caption-monitor/releases/tag/v1.0.0) · [todas as versões](https://github.com/GabrielSmash/arib-caption-monitor/releases)
>
> Como instalar: veja [Instalação](#instalação-qualquer-distribuição-linux).

- Recebe MPEG-TS por UDP (unicast/multicast, RTP detectado) **ou reproduz arquivos** `.ts`/`.m2ts`
  (188, 192 e 204 bytes) em tempo real, com o ritmo dado pelo PCR (ou PTS, em arquivos só de legenda).
- Indica se os pacotes estão chegando (pkt/s, bitrate, origem, tempo desde o último datagrama).
- Reproduz vídeo e áudio quando o stream os contém (FFmpeg do sistema + Qt Multimedia), com a legenda por cima.
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

## Instalação (qualquer distribuição Linux)

O app é distribuído como **Flatpak**: um arquivo `arib-caption-monitor-<versão>.flatpak` publicado nas
releases do repositório. Funciona em Ubuntu, Debian, Fedora, Mint, Arch etc.

1. Baixe o `.flatpak` da [versão mais recente](https://github.com/GabrielSmash/arib-caption-monitor/releases/latest). Pelo terminal (o repositório é privado,
   então o download usa o seu login do GitHub CLI):
   ```bash
   gh release download v1.0.0 -R GabrielSmash/arib-caption-monitor -p '*.flatpak'
   ```
2. Tenha o Flatpak e o Flathub configurados (já vêm na maioria das distros; senão,
   veja https://flathub.org/setup):
   ```bash
   flatpak remote-add --if-not-exists --user flathub https://dl.flathub.org/repo/flathub.flatpakrepo
   ```
3. Instale o arquivo baixado (na primeira vez ele baixa também o runtime KDE/Qt do Flathub):
   ```bash
   flatpak install --user arib-caption-monitor-1.0.0.flatpak
   ```
4. Abra pelo menu de aplicativos (**ARIB Caption Monitor**) ou pelo terminal:
   ```bash
   flatpak run io.github.gabrielsmash.AribCaptionMonitor            # escuta UDP (porta salva, padrão 1526)
   flatpak run io.github.gabrielsmash.AribCaptionMonitor arquivo.ts # reproduz um arquivo
   ```

Para atualizar, instale o `.flatpak` da versão nova da mesma forma. Para remover:
`flatpak uninstall io.github.gabrielsmash.AribCaptionMonitor`.

Permissões do pacote: rede (recepção UDP), áudio, vídeo/GPU e **leitura** da pasta pessoal e de
`/media` e `/run/media` (pendrives). Arquivos em outros lugares podem ser abertos pelo botão "Abrir .ts…".

### Gerar o pacote

```bash
flatpak install --user flathub org.flatpak.Builder   # uma vez
./packaging/flatpak/build.sh                         # gera dist/arib-caption-monitor-<versão>.flatpak
```

O build usa o runtime `org.kde.Platform//6.10` (Qt 6.10 e FFmpeg; H.264/AAC vêm da extensão
`org.freedesktop.Platform.codecs-extra`, instalada automaticamente).

## Desenvolvimento

### Dependências

```bash
sudo apt install build-essential cmake pkg-config qt6-base-dev qt6-multimedia-dev libfreetype-dev libfontconfig-dev fonts-dejavu \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev
```

A libaribcaption **não** precisa estar instalada: o CMake baixa uma versão fixa do upstream, aplica
`patches/libaribcaption-deferred-wrap.patch` e compila estática junto com o app (precisa de `git` e internet
no primeiro `cmake`). Detalhes em `patches/README.md`.

### Build

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
ctest --test-dir build        # layout (quebra de linha) e validação, com vetores reais (Globo, SkylarBox)
```

### Uso

```bash
./build/arib-caption-monitor                  # porta salva (padrão 1526), unicast
./build/arib-caption-monitor -p 1526 -m 239.1.1.1   # multicast
./build/arib-caption-monitor --file gravacao.ts       # arquivo (ou botão "Abrir .ts…" / arrastar para a janela)
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
- Arquivos: sem busca (seek); "Repetir" volta ao início. Vídeo entrelaçado é exibido sem desentrelaçar.
- O app força o backend de áudio PulseAudio do Qt (`QT_AUDIO_BACKEND`), porque o backend PipeWire do Qt 6.10
  não lista saídas Bluetooth.
