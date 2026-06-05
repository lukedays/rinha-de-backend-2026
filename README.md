# Rinha de Backend 2026 — detecção de fraude (C + ASM)

Detector de fraude por busca vetorial k-NN (k=5) sobre 3 milhões de vetores de
14 dimensões, em **C puro com kernel SIMD em assembly AVX2**. Sem frameworks,
sem runtime, sem banco — só syscalls Linux e um índice mmap.

## Arquitetura

```
cliente --TCP 9999--> [ lb ] --SCM_RIGHTS (fd-passing)--> [ api1 ] [ api2 ]
                       Unix socket, round-robin            epoll + busca exata
```

- **LB (`lb.c`)**: aceita TCP na 9999 e passa o *file descriptor* da conexão para
  as APIs via Unix socket (`SCM_RIGHTS`). Não inspeciona nem proxia o payload —
  só reparte o socket. Custa ~0.06 CPU.
- **API (`server.c`)**: servidor HTTP/1.1 epoll que recebe fds prontos do LB,
  com keep-alive e respostas pré-serializadas. Zero alocação no caminho de
  request.
- **Busca (`search.c` + `distance_avx2.S`)**: IVF exato. k-means agrupa os 3M
  vetores em 2048 clusters com AABB; a query visita só os clusters cujo *lower
  bound* (AABB, em AVX2) pode conter um dos 5 vizinhos, com:
  - **kernel ASM 8-lanes** (`vpmaddwd` pair-interleaved) para a distância;
  - **early-stop por AABB** + **chromatic deferral** (para quando a decisão
    `approved` já está determinada). Visita ~7 dos 2048 clusters; escaneia 0,34%
    dos vetores.
- **Quantização int16 escala 10000**: lossless para os refs de 4 casas decimais,
  então a busca reproduz exatamente o gabarito (k-NN euclidiano em float).

## Resultados (busca isolada, sob 0.45 CPU)

- p50 **24µs**, p99 **163µs** por consulta.
- Detecção: **0 divergência** vs brute force exato.
- Memória: **~0,6 MB por API** (índice via mmap read-only, compartilhado).

## Build e execução

```sh
# índice (offline): references.json.gz -> data/index_v2.bin
cd c && make build_index
./build_index ../resources/references.json.gz ../data/index_v2.bin

# stack completo
docker compose up --build
# testa
curl localhost:9999/ready
```

## Layout

```
c/
  server.c          API: epoll, HTTP, fd-passing
  lb.c              load balancer fd-passing
  search.c          busca IVF exata (AVX2 lb + chromatic deferral)
  distance_avx2.S   kernel ASM 8-lanes (distancia euclidiana int16)
  build_index.c     builder offline (gzip + k-means + AABB + chunks)
  parser.c          parser JSON do payload (zero-alloc)
  vectorize.h       vetorizacao 14 dims + quantizacao int16
  timestamp.h       parse ISO-8601 sem alocacao
  selftest.c        valida paridade vs brute force
  bench.c           mede latencia da busca
data/index_v2.bin   indice pre-computado (int16 + AABB + chunks)
```

Licença MIT.
