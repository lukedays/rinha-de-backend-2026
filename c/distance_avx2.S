# Kernel AVX2 8-lanes: distancia euclidiana ao quadrado de 8 vetores de
# referencia por chunk, contra a query. Layout pair-interleaved (int16):
#   chunk[p*16 + lane*2 + r] = vetor_lane[2p + r], p=0..6, r=0..1, lane=0..7
#
#   void dist_chunks_i16(const int32_t *qpairs, const int16_t *chunks,
#                        long nchunks, int32_t *out);
#
# qpairs[p] = (uint16_t)q[2p] | ((uint16_t)q[2p+1] << 16)   (7 pares)
# out[chunk*8 + lane] = sum_{d<14} (vetor_lane[d] - q[d])^2
#
# Cada lane acumula sua propria distancia (sem reducao horizontal): vpmaddwd
# soma o par (2p,2p+1) por lane; 7 pares somados dao a distancia das 14 dims.
# System V AMD64: rdi=qpairs, rsi=chunks, rdx=nchunks, rcx=out.

    .intel_syntax noprefix
    .text
    .globl  dist_chunks_i16
    .type   dist_chunks_i16, @function
dist_chunks_i16:
    test    rdx, rdx
    jle     .Ldone

    vpbroadcastd ymm9,  [rdi]          # query pares 0..6 (cada lane: [q2p, q2p+1])
    vpbroadcastd ymm10, [rdi + 4]
    vpbroadcastd ymm11, [rdi + 8]
    vpbroadcastd ymm12, [rdi + 12]
    vpbroadcastd ymm13, [rdi + 16]
    vpbroadcastd ymm14, [rdi + 20]
    vpbroadcastd ymm15, [rdi + 24]

.Lloop:
    vmovdqu  ymm0, [rsi]               # par 0 (dims 0,1 dos 8 lanes)
    vpsubw   ymm0, ymm0, ymm9
    vpmaddwd ymm0, ymm0, ymm0         # 8x int32: diff0^2 + diff1^2 por lane

    vmovdqu  ymm1, [rsi + 32]          # par 1 (dims 2,3)
    vpsubw   ymm1, ymm1, ymm10
    vpmaddwd ymm1, ymm1, ymm1
    vpaddd   ymm0, ymm0, ymm1

    vmovdqu  ymm1, [rsi + 64]          # par 2 (dims 4,5)
    vpsubw   ymm1, ymm1, ymm11
    vpmaddwd ymm1, ymm1, ymm1
    vpaddd   ymm0, ymm0, ymm1

    vmovdqu  ymm1, [rsi + 96]          # par 3 (dims 6,7)
    vpsubw   ymm1, ymm1, ymm12
    vpmaddwd ymm1, ymm1, ymm1
    vpaddd   ymm0, ymm0, ymm1

    vmovdqu  ymm1, [rsi + 128]         # par 4 (dims 8,9)
    vpsubw   ymm1, ymm1, ymm13
    vpmaddwd ymm1, ymm1, ymm1
    vpaddd   ymm0, ymm0, ymm1

    vmovdqu  ymm1, [rsi + 160]         # par 5 (dims 10,11)
    vpsubw   ymm1, ymm1, ymm14
    vpmaddwd ymm1, ymm1, ymm1
    vpaddd   ymm0, ymm0, ymm1

    vmovdqu  ymm1, [rsi + 192]         # par 6 (dims 12,13)
    vpsubw   ymm1, ymm1, ymm15
    vpmaddwd ymm1, ymm1, ymm1
    vpaddd   ymm0, ymm0, ymm1

    vmovdqu  [rcx], ymm0               # 8 distancias int32
    add      rsi, 224                  # proximo chunk (112 int16)
    add      rcx, 32
    dec      rdx
    jnz      .Lloop

.Ldone:
    vzeroupper
    ret

    .section .note.GNU-stack,"",@progbits
