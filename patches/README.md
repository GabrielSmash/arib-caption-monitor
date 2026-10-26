# Patches de dependências

## libaribcaption-deferred-wrap.patch

Base: libaribcaption `37dde0f32652c32aa98bf92278944d1cfb2e7354` (aplicado pelo CMake via FetchContent).

**Problema:** o decoder quebra a linha assim que um caractere preenche a linha até a borda direita da área
de exibição (SDF). Quando o emissor envia uma linha cheia seguida de `APR` — prática usada por emissoras
brasileiras (ex.: Globo) e pelo encoder SkylarBox — a linha seguinte desce duas posições e pode sair da área.
Issue upstream: https://github.com/xqq/libaribcaption/issues/27

**Correção:** depois de escrever um caractere, o avanço só marca a quebra como pendente; ela acontece quando o
próximo caractere for escrito. Qualquer movimento explícito (`APR`, `APD`, `APF`, `APS`, `ACPS`, `SDP`, `CS`)
cancela a pendência.

Base normativa: ARIB TR-B14 §4.4.4 descreve a quebra ligada à exibição do caractere seguinte. Não confirmado
no texto da ABNT NBR 15606-1 (não disponível no projeto); validado contra stream real da Globo.

Teste: `ctest --test-dir build` (`tests/test_caption_layout.cpp`).

Quando o upstream corrigir a issue, remova o `PATCH_COMMAND` do `CMakeLists.txt` e atualize o `GIT_TAG`.
