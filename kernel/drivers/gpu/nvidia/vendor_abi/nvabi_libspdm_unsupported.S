/* 第 3 阶段的 libspdm ABI 安全失败边界。具体声明由 nvabi_contracts.c 检查。 */

    .section .text.nvabi_libspdm_unsupported,"ax",@progbits
    .extern nvabi_log_unsupported
    .p2align 4

    .globl libspdm_aead_aes_gcm_decrypt
    .type libspdm_aead_aes_gcm_decrypt, @function
libspdm_aead_aes_gcm_decrypt:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_aead_aes_gcm_decrypt(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_aead_aes_gcm_decrypt, .-libspdm_aead_aes_gcm_decrypt

    .globl libspdm_aead_aes_gcm_decrypt_prealloc
    .type libspdm_aead_aes_gcm_decrypt_prealloc, @function
libspdm_aead_aes_gcm_decrypt_prealloc:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_aead_aes_gcm_decrypt_prealloc(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_aead_aes_gcm_decrypt_prealloc, .-libspdm_aead_aes_gcm_decrypt_prealloc

    .globl libspdm_aead_aes_gcm_encrypt
    .type libspdm_aead_aes_gcm_encrypt, @function
libspdm_aead_aes_gcm_encrypt:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_aead_aes_gcm_encrypt(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_aead_aes_gcm_encrypt, .-libspdm_aead_aes_gcm_encrypt

    .globl libspdm_aead_aes_gcm_encrypt_prealloc
    .type libspdm_aead_aes_gcm_encrypt_prealloc, @function
libspdm_aead_aes_gcm_encrypt_prealloc:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_aead_aes_gcm_encrypt_prealloc(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_aead_aes_gcm_encrypt_prealloc, .-libspdm_aead_aes_gcm_encrypt_prealloc

    .globl libspdm_aead_chacha20_poly1305_decrypt
    .type libspdm_aead_chacha20_poly1305_decrypt, @function
libspdm_aead_chacha20_poly1305_decrypt:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_aead_chacha20_poly1305_decrypt(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_aead_chacha20_poly1305_decrypt, .-libspdm_aead_chacha20_poly1305_decrypt

    .globl libspdm_aead_chacha20_poly1305_encrypt
    .type libspdm_aead_chacha20_poly1305_encrypt, @function
libspdm_aead_chacha20_poly1305_encrypt:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_aead_chacha20_poly1305_encrypt(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_aead_chacha20_poly1305_encrypt, .-libspdm_aead_chacha20_poly1305_encrypt

    .globl libspdm_aead_free
    .type libspdm_aead_free, @function
libspdm_aead_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_aead_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_aead_free, .-libspdm_aead_free

    .globl libspdm_aead_gcm_prealloc
    .type libspdm_aead_gcm_prealloc, @function
libspdm_aead_gcm_prealloc:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_aead_gcm_prealloc(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_aead_gcm_prealloc, .-libspdm_aead_gcm_prealloc

    .globl libspdm_aead_sm4_gcm_decrypt
    .type libspdm_aead_sm4_gcm_decrypt, @function
libspdm_aead_sm4_gcm_decrypt:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_aead_sm4_gcm_decrypt(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_aead_sm4_gcm_decrypt, .-libspdm_aead_sm4_gcm_decrypt

    .globl libspdm_aead_sm4_gcm_encrypt
    .type libspdm_aead_sm4_gcm_encrypt, @function
libspdm_aead_sm4_gcm_encrypt:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_aead_sm4_gcm_encrypt(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_aead_sm4_gcm_encrypt, .-libspdm_aead_sm4_gcm_encrypt

    .globl libspdm_asn1_get_tag
    .type libspdm_asn1_get_tag, @function
libspdm_asn1_get_tag:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_asn1_get_tag(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_asn1_get_tag, .-libspdm_asn1_get_tag

    .globl libspdm_check_crypto_backend
    .type libspdm_check_crypto_backend, @function
libspdm_check_crypto_backend:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_check_crypto_backend(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_check_crypto_backend, .-libspdm_check_crypto_backend

    .globl libspdm_decode_base64
    .type libspdm_decode_base64, @function
libspdm_decode_base64:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_decode_base64(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_decode_base64, .-libspdm_decode_base64

    .globl libspdm_dh_compute_key
    .type libspdm_dh_compute_key, @function
libspdm_dh_compute_key:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_dh_compute_key(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_dh_compute_key, .-libspdm_dh_compute_key

    .globl libspdm_dh_free
    .type libspdm_dh_free, @function
libspdm_dh_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_dh_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_dh_free, .-libspdm_dh_free

    .globl libspdm_dh_generate_key
    .type libspdm_dh_generate_key, @function
libspdm_dh_generate_key:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_dh_generate_key(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_dh_generate_key, .-libspdm_dh_generate_key

    .globl libspdm_dh_new_by_nid
    .type libspdm_dh_new_by_nid, @function
libspdm_dh_new_by_nid:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_dh_new_by_nid(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_dh_new_by_nid, .-libspdm_dh_new_by_nid

    .globl libspdm_ec_compute_key
    .type libspdm_ec_compute_key, @function
libspdm_ec_compute_key:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_ec_compute_key(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_ec_compute_key, .-libspdm_ec_compute_key

    .globl libspdm_ec_free
    .type libspdm_ec_free, @function
libspdm_ec_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_ec_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_ec_free, .-libspdm_ec_free

    .globl libspdm_ec_generate_key
    .type libspdm_ec_generate_key, @function
libspdm_ec_generate_key:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_ec_generate_key(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_ec_generate_key, .-libspdm_ec_generate_key

    .globl libspdm_ec_get_public_key_from_x509
    .type libspdm_ec_get_public_key_from_x509, @function
libspdm_ec_get_public_key_from_x509:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_ec_get_public_key_from_x509(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_ec_get_public_key_from_x509, .-libspdm_ec_get_public_key_from_x509

    .globl libspdm_ec_new_by_nid
    .type libspdm_ec_new_by_nid, @function
libspdm_ec_new_by_nid:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_ec_new_by_nid(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_ec_new_by_nid, .-libspdm_ec_new_by_nid

    .globl libspdm_ecd_free
    .type libspdm_ecd_free, @function
libspdm_ecd_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_ecd_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_ecd_free, .-libspdm_ecd_free

    .globl libspdm_ecd_get_public_key_from_der
    .type libspdm_ecd_get_public_key_from_der, @function
libspdm_ecd_get_public_key_from_der:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_ecd_get_public_key_from_der(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_ecd_get_public_key_from_der, .-libspdm_ecd_get_public_key_from_der

    .globl libspdm_ecd_get_public_key_from_x509
    .type libspdm_ecd_get_public_key_from_x509, @function
libspdm_ecd_get_public_key_from_x509:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_ecd_get_public_key_from_x509(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_ecd_get_public_key_from_x509, .-libspdm_ecd_get_public_key_from_x509

    .globl libspdm_ecdsa_sign
    .type libspdm_ecdsa_sign, @function
libspdm_ecdsa_sign:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_ecdsa_sign(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_ecdsa_sign, .-libspdm_ecdsa_sign

    .globl libspdm_ecdsa_verify
    .type libspdm_ecdsa_verify, @function
libspdm_ecdsa_verify:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_ecdsa_verify(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_ecdsa_verify, .-libspdm_ecdsa_verify

    .globl libspdm_eddsa_sign
    .type libspdm_eddsa_sign, @function
libspdm_eddsa_sign:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_eddsa_sign(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_eddsa_sign, .-libspdm_eddsa_sign

    .globl libspdm_eddsa_verify
    .type libspdm_eddsa_verify, @function
libspdm_eddsa_verify:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_eddsa_verify(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_eddsa_verify, .-libspdm_eddsa_verify

    .globl libspdm_encode_base64
    .type libspdm_encode_base64, @function
libspdm_encode_base64:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_encode_base64(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_encode_base64, .-libspdm_encode_base64

    .globl libspdm_get_encap_response_challenge_auth
    .type libspdm_get_encap_response_challenge_auth, @function
libspdm_get_encap_response_challenge_auth:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_get_encap_response_challenge_auth(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x80010002, %eax
    popq %rbp
    ret
    .size libspdm_get_encap_response_challenge_auth, .-libspdm_get_encap_response_challenge_auth

    .globl libspdm_hkdf_sha256_expand
    .type libspdm_hkdf_sha256_expand, @function
libspdm_hkdf_sha256_expand:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hkdf_sha256_expand(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hkdf_sha256_expand, .-libspdm_hkdf_sha256_expand

    .globl libspdm_hkdf_sha256_extract
    .type libspdm_hkdf_sha256_extract, @function
libspdm_hkdf_sha256_extract:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hkdf_sha256_extract(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hkdf_sha256_extract, .-libspdm_hkdf_sha256_extract

    .globl libspdm_hkdf_sha3_256_expand
    .type libspdm_hkdf_sha3_256_expand, @function
libspdm_hkdf_sha3_256_expand:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hkdf_sha3_256_expand(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hkdf_sha3_256_expand, .-libspdm_hkdf_sha3_256_expand

    .globl libspdm_hkdf_sha3_256_extract
    .type libspdm_hkdf_sha3_256_extract, @function
libspdm_hkdf_sha3_256_extract:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hkdf_sha3_256_extract(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hkdf_sha3_256_extract, .-libspdm_hkdf_sha3_256_extract

    .globl libspdm_hkdf_sha3_384_expand
    .type libspdm_hkdf_sha3_384_expand, @function
libspdm_hkdf_sha3_384_expand:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hkdf_sha3_384_expand(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hkdf_sha3_384_expand, .-libspdm_hkdf_sha3_384_expand

    .globl libspdm_hkdf_sha3_384_extract
    .type libspdm_hkdf_sha3_384_extract, @function
libspdm_hkdf_sha3_384_extract:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hkdf_sha3_384_extract(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hkdf_sha3_384_extract, .-libspdm_hkdf_sha3_384_extract

    .globl libspdm_hkdf_sha3_512_expand
    .type libspdm_hkdf_sha3_512_expand, @function
libspdm_hkdf_sha3_512_expand:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hkdf_sha3_512_expand(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hkdf_sha3_512_expand, .-libspdm_hkdf_sha3_512_expand

    .globl libspdm_hkdf_sha3_512_extract
    .type libspdm_hkdf_sha3_512_extract, @function
libspdm_hkdf_sha3_512_extract:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hkdf_sha3_512_extract(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hkdf_sha3_512_extract, .-libspdm_hkdf_sha3_512_extract

    .globl libspdm_hkdf_sha384_expand
    .type libspdm_hkdf_sha384_expand, @function
libspdm_hkdf_sha384_expand:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hkdf_sha384_expand(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hkdf_sha384_expand, .-libspdm_hkdf_sha384_expand

    .globl libspdm_hkdf_sha384_extract
    .type libspdm_hkdf_sha384_extract, @function
libspdm_hkdf_sha384_extract:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hkdf_sha384_extract(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hkdf_sha384_extract, .-libspdm_hkdf_sha384_extract

    .globl libspdm_hkdf_sha512_expand
    .type libspdm_hkdf_sha512_expand, @function
libspdm_hkdf_sha512_expand:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hkdf_sha512_expand(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hkdf_sha512_expand, .-libspdm_hkdf_sha512_expand

    .globl libspdm_hkdf_sha512_extract
    .type libspdm_hkdf_sha512_extract, @function
libspdm_hkdf_sha512_extract:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hkdf_sha512_extract(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hkdf_sha512_extract, .-libspdm_hkdf_sha512_extract

    .globl libspdm_hkdf_sm3_256_expand
    .type libspdm_hkdf_sm3_256_expand, @function
libspdm_hkdf_sm3_256_expand:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hkdf_sm3_256_expand(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hkdf_sm3_256_expand, .-libspdm_hkdf_sm3_256_expand

    .globl libspdm_hkdf_sm3_256_extract
    .type libspdm_hkdf_sm3_256_extract, @function
libspdm_hkdf_sm3_256_extract:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hkdf_sm3_256_extract(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hkdf_sm3_256_extract, .-libspdm_hkdf_sm3_256_extract

    .globl libspdm_hmac_sha256_all
    .type libspdm_hmac_sha256_all, @function
libspdm_hmac_sha256_all:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha256_all(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha256_all, .-libspdm_hmac_sha256_all

    .globl libspdm_hmac_sha256_duplicate
    .type libspdm_hmac_sha256_duplicate, @function
libspdm_hmac_sha256_duplicate:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha256_duplicate(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha256_duplicate, .-libspdm_hmac_sha256_duplicate

    .globl libspdm_hmac_sha256_final
    .type libspdm_hmac_sha256_final, @function
libspdm_hmac_sha256_final:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha256_final(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha256_final, .-libspdm_hmac_sha256_final

    .globl libspdm_hmac_sha256_free
    .type libspdm_hmac_sha256_free, @function
libspdm_hmac_sha256_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha256_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_hmac_sha256_free, .-libspdm_hmac_sha256_free

    .globl libspdm_hmac_sha256_new
    .type libspdm_hmac_sha256_new, @function
libspdm_hmac_sha256_new:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha256_new(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha256_new, .-libspdm_hmac_sha256_new

    .globl libspdm_hmac_sha256_set_key
    .type libspdm_hmac_sha256_set_key, @function
libspdm_hmac_sha256_set_key:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha256_set_key(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha256_set_key, .-libspdm_hmac_sha256_set_key

    .globl libspdm_hmac_sha256_update
    .type libspdm_hmac_sha256_update, @function
libspdm_hmac_sha256_update:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha256_update(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha256_update, .-libspdm_hmac_sha256_update

    .globl libspdm_hmac_sha3_256_all
    .type libspdm_hmac_sha3_256_all, @function
libspdm_hmac_sha3_256_all:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_256_all(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha3_256_all, .-libspdm_hmac_sha3_256_all

    .globl libspdm_hmac_sha3_256_duplicate
    .type libspdm_hmac_sha3_256_duplicate, @function
libspdm_hmac_sha3_256_duplicate:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_256_duplicate(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha3_256_duplicate, .-libspdm_hmac_sha3_256_duplicate

    .globl libspdm_hmac_sha3_256_final
    .type libspdm_hmac_sha3_256_final, @function
libspdm_hmac_sha3_256_final:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_256_final(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha3_256_final, .-libspdm_hmac_sha3_256_final

    .globl libspdm_hmac_sha3_256_free
    .type libspdm_hmac_sha3_256_free, @function
libspdm_hmac_sha3_256_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_256_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_hmac_sha3_256_free, .-libspdm_hmac_sha3_256_free

    .globl libspdm_hmac_sha3_256_new
    .type libspdm_hmac_sha3_256_new, @function
libspdm_hmac_sha3_256_new:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_256_new(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha3_256_new, .-libspdm_hmac_sha3_256_new

    .globl libspdm_hmac_sha3_256_set_key
    .type libspdm_hmac_sha3_256_set_key, @function
libspdm_hmac_sha3_256_set_key:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_256_set_key(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha3_256_set_key, .-libspdm_hmac_sha3_256_set_key

    .globl libspdm_hmac_sha3_256_update
    .type libspdm_hmac_sha3_256_update, @function
libspdm_hmac_sha3_256_update:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_256_update(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha3_256_update, .-libspdm_hmac_sha3_256_update

    .globl libspdm_hmac_sha3_384_all
    .type libspdm_hmac_sha3_384_all, @function
libspdm_hmac_sha3_384_all:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_384_all(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha3_384_all, .-libspdm_hmac_sha3_384_all

    .globl libspdm_hmac_sha3_384_duplicate
    .type libspdm_hmac_sha3_384_duplicate, @function
libspdm_hmac_sha3_384_duplicate:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_384_duplicate(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha3_384_duplicate, .-libspdm_hmac_sha3_384_duplicate

    .globl libspdm_hmac_sha3_384_final
    .type libspdm_hmac_sha3_384_final, @function
libspdm_hmac_sha3_384_final:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_384_final(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha3_384_final, .-libspdm_hmac_sha3_384_final

    .globl libspdm_hmac_sha3_384_free
    .type libspdm_hmac_sha3_384_free, @function
libspdm_hmac_sha3_384_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_384_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_hmac_sha3_384_free, .-libspdm_hmac_sha3_384_free

    .globl libspdm_hmac_sha3_384_new
    .type libspdm_hmac_sha3_384_new, @function
libspdm_hmac_sha3_384_new:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_384_new(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha3_384_new, .-libspdm_hmac_sha3_384_new

    .globl libspdm_hmac_sha3_384_set_key
    .type libspdm_hmac_sha3_384_set_key, @function
libspdm_hmac_sha3_384_set_key:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_384_set_key(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha3_384_set_key, .-libspdm_hmac_sha3_384_set_key

    .globl libspdm_hmac_sha3_384_update
    .type libspdm_hmac_sha3_384_update, @function
libspdm_hmac_sha3_384_update:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_384_update(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha3_384_update, .-libspdm_hmac_sha3_384_update

    .globl libspdm_hmac_sha3_512_all
    .type libspdm_hmac_sha3_512_all, @function
libspdm_hmac_sha3_512_all:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_512_all(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha3_512_all, .-libspdm_hmac_sha3_512_all

    .globl libspdm_hmac_sha3_512_duplicate
    .type libspdm_hmac_sha3_512_duplicate, @function
libspdm_hmac_sha3_512_duplicate:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_512_duplicate(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha3_512_duplicate, .-libspdm_hmac_sha3_512_duplicate

    .globl libspdm_hmac_sha3_512_final
    .type libspdm_hmac_sha3_512_final, @function
libspdm_hmac_sha3_512_final:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_512_final(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha3_512_final, .-libspdm_hmac_sha3_512_final

    .globl libspdm_hmac_sha3_512_free
    .type libspdm_hmac_sha3_512_free, @function
libspdm_hmac_sha3_512_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_512_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_hmac_sha3_512_free, .-libspdm_hmac_sha3_512_free

    .globl libspdm_hmac_sha3_512_new
    .type libspdm_hmac_sha3_512_new, @function
libspdm_hmac_sha3_512_new:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_512_new(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha3_512_new, .-libspdm_hmac_sha3_512_new

    .globl libspdm_hmac_sha3_512_set_key
    .type libspdm_hmac_sha3_512_set_key, @function
libspdm_hmac_sha3_512_set_key:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_512_set_key(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha3_512_set_key, .-libspdm_hmac_sha3_512_set_key

    .globl libspdm_hmac_sha3_512_update
    .type libspdm_hmac_sha3_512_update, @function
libspdm_hmac_sha3_512_update:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha3_512_update(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha3_512_update, .-libspdm_hmac_sha3_512_update

    .globl libspdm_hmac_sha384_all
    .type libspdm_hmac_sha384_all, @function
libspdm_hmac_sha384_all:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha384_all(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha384_all, .-libspdm_hmac_sha384_all

    .globl libspdm_hmac_sha384_duplicate
    .type libspdm_hmac_sha384_duplicate, @function
libspdm_hmac_sha384_duplicate:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha384_duplicate(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha384_duplicate, .-libspdm_hmac_sha384_duplicate

    .globl libspdm_hmac_sha384_final
    .type libspdm_hmac_sha384_final, @function
libspdm_hmac_sha384_final:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha384_final(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha384_final, .-libspdm_hmac_sha384_final

    .globl libspdm_hmac_sha384_free
    .type libspdm_hmac_sha384_free, @function
libspdm_hmac_sha384_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha384_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_hmac_sha384_free, .-libspdm_hmac_sha384_free

    .globl libspdm_hmac_sha384_new
    .type libspdm_hmac_sha384_new, @function
libspdm_hmac_sha384_new:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha384_new(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha384_new, .-libspdm_hmac_sha384_new

    .globl libspdm_hmac_sha384_set_key
    .type libspdm_hmac_sha384_set_key, @function
libspdm_hmac_sha384_set_key:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha384_set_key(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha384_set_key, .-libspdm_hmac_sha384_set_key

    .globl libspdm_hmac_sha384_update
    .type libspdm_hmac_sha384_update, @function
libspdm_hmac_sha384_update:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha384_update(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha384_update, .-libspdm_hmac_sha384_update

    .globl libspdm_hmac_sha512_all
    .type libspdm_hmac_sha512_all, @function
libspdm_hmac_sha512_all:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha512_all(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha512_all, .-libspdm_hmac_sha512_all

    .globl libspdm_hmac_sha512_duplicate
    .type libspdm_hmac_sha512_duplicate, @function
libspdm_hmac_sha512_duplicate:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha512_duplicate(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha512_duplicate, .-libspdm_hmac_sha512_duplicate

    .globl libspdm_hmac_sha512_final
    .type libspdm_hmac_sha512_final, @function
libspdm_hmac_sha512_final:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha512_final(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha512_final, .-libspdm_hmac_sha512_final

    .globl libspdm_hmac_sha512_free
    .type libspdm_hmac_sha512_free, @function
libspdm_hmac_sha512_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha512_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_hmac_sha512_free, .-libspdm_hmac_sha512_free

    .globl libspdm_hmac_sha512_new
    .type libspdm_hmac_sha512_new, @function
libspdm_hmac_sha512_new:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha512_new(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha512_new, .-libspdm_hmac_sha512_new

    .globl libspdm_hmac_sha512_set_key
    .type libspdm_hmac_sha512_set_key, @function
libspdm_hmac_sha512_set_key:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha512_set_key(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha512_set_key, .-libspdm_hmac_sha512_set_key

    .globl libspdm_hmac_sha512_update
    .type libspdm_hmac_sha512_update, @function
libspdm_hmac_sha512_update:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sha512_update(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sha512_update, .-libspdm_hmac_sha512_update

    .globl libspdm_hmac_sm3_256_all
    .type libspdm_hmac_sm3_256_all, @function
libspdm_hmac_sm3_256_all:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sm3_256_all(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sm3_256_all, .-libspdm_hmac_sm3_256_all

    .globl libspdm_hmac_sm3_256_duplicate
    .type libspdm_hmac_sm3_256_duplicate, @function
libspdm_hmac_sm3_256_duplicate:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sm3_256_duplicate(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sm3_256_duplicate, .-libspdm_hmac_sm3_256_duplicate

    .globl libspdm_hmac_sm3_256_final
    .type libspdm_hmac_sm3_256_final, @function
libspdm_hmac_sm3_256_final:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sm3_256_final(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sm3_256_final, .-libspdm_hmac_sm3_256_final

    .globl libspdm_hmac_sm3_256_free
    .type libspdm_hmac_sm3_256_free, @function
libspdm_hmac_sm3_256_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sm3_256_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_hmac_sm3_256_free, .-libspdm_hmac_sm3_256_free

    .globl libspdm_hmac_sm3_256_new
    .type libspdm_hmac_sm3_256_new, @function
libspdm_hmac_sm3_256_new:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sm3_256_new(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sm3_256_new, .-libspdm_hmac_sm3_256_new

    .globl libspdm_hmac_sm3_256_set_key
    .type libspdm_hmac_sm3_256_set_key, @function
libspdm_hmac_sm3_256_set_key:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sm3_256_set_key(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sm3_256_set_key, .-libspdm_hmac_sm3_256_set_key

    .globl libspdm_hmac_sm3_256_update
    .type libspdm_hmac_sm3_256_update, @function
libspdm_hmac_sm3_256_update:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_hmac_sm3_256_update(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_hmac_sm3_256_update, .-libspdm_hmac_sm3_256_update

    .globl libspdm_random_bytes
    .type libspdm_random_bytes, @function
libspdm_random_bytes:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_random_bytes(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_random_bytes, .-libspdm_random_bytes

    .globl libspdm_rsa_free
    .type libspdm_rsa_free, @function
libspdm_rsa_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_rsa_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_rsa_free, .-libspdm_rsa_free

    .globl libspdm_rsa_get_public_key_from_x509
    .type libspdm_rsa_get_public_key_from_x509, @function
libspdm_rsa_get_public_key_from_x509:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_rsa_get_public_key_from_x509(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_rsa_get_public_key_from_x509, .-libspdm_rsa_get_public_key_from_x509

    .globl libspdm_rsa_new
    .type libspdm_rsa_new, @function
libspdm_rsa_new:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_rsa_new(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_rsa_new, .-libspdm_rsa_new

    .globl libspdm_rsa_pkcs1_sign_with_nid
    .type libspdm_rsa_pkcs1_sign_with_nid, @function
libspdm_rsa_pkcs1_sign_with_nid:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_rsa_pkcs1_sign_with_nid(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_rsa_pkcs1_sign_with_nid, .-libspdm_rsa_pkcs1_sign_with_nid

    .globl libspdm_rsa_pkcs1_verify_with_nid
    .type libspdm_rsa_pkcs1_verify_with_nid, @function
libspdm_rsa_pkcs1_verify_with_nid:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_rsa_pkcs1_verify_with_nid(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_rsa_pkcs1_verify_with_nid, .-libspdm_rsa_pkcs1_verify_with_nid

    .globl libspdm_rsa_pss_sign
    .type libspdm_rsa_pss_sign, @function
libspdm_rsa_pss_sign:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_rsa_pss_sign(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_rsa_pss_sign, .-libspdm_rsa_pss_sign

    .globl libspdm_rsa_pss_verify
    .type libspdm_rsa_pss_verify, @function
libspdm_rsa_pss_verify:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_rsa_pss_verify(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_rsa_pss_verify, .-libspdm_rsa_pss_verify

    .globl libspdm_rsa_set_key
    .type libspdm_rsa_set_key, @function
libspdm_rsa_set_key:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_rsa_set_key(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_rsa_set_key, .-libspdm_rsa_set_key

    .globl libspdm_send_receive_psk_exchange
    .type libspdm_send_receive_psk_exchange, @function
libspdm_send_receive_psk_exchange:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_send_receive_psk_exchange(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x80010002, %eax
    popq %rbp
    ret
    .size libspdm_send_receive_psk_exchange, .-libspdm_send_receive_psk_exchange

    .globl libspdm_send_receive_psk_exchange_ex
    .type libspdm_send_receive_psk_exchange_ex, @function
libspdm_send_receive_psk_exchange_ex:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_send_receive_psk_exchange_ex(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x80010002, %eax
    popq %rbp
    ret
    .size libspdm_send_receive_psk_exchange_ex, .-libspdm_send_receive_psk_exchange_ex

    .globl libspdm_send_receive_psk_finish
    .type libspdm_send_receive_psk_finish, @function
libspdm_send_receive_psk_finish:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_send_receive_psk_finish(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    movl $0x80010002, %eax
    popq %rbp
    ret
    .size libspdm_send_receive_psk_finish, .-libspdm_send_receive_psk_finish

    .globl libspdm_sha256_duplicate
    .type libspdm_sha256_duplicate, @function
libspdm_sha256_duplicate:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha256_duplicate(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha256_duplicate, .-libspdm_sha256_duplicate

    .globl libspdm_sha256_final
    .type libspdm_sha256_final, @function
libspdm_sha256_final:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha256_final(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha256_final, .-libspdm_sha256_final

    .globl libspdm_sha256_free
    .type libspdm_sha256_free, @function
libspdm_sha256_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha256_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_sha256_free, .-libspdm_sha256_free

    .globl libspdm_sha256_hash_all
    .type libspdm_sha256_hash_all, @function
libspdm_sha256_hash_all:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha256_hash_all(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha256_hash_all, .-libspdm_sha256_hash_all

    .globl libspdm_sha256_init
    .type libspdm_sha256_init, @function
libspdm_sha256_init:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha256_init(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha256_init, .-libspdm_sha256_init

    .globl libspdm_sha256_new
    .type libspdm_sha256_new, @function
libspdm_sha256_new:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha256_new(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha256_new, .-libspdm_sha256_new

    .globl libspdm_sha256_update
    .type libspdm_sha256_update, @function
libspdm_sha256_update:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha256_update(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha256_update, .-libspdm_sha256_update

    .globl libspdm_sha3_256_duplicate
    .type libspdm_sha3_256_duplicate, @function
libspdm_sha3_256_duplicate:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_256_duplicate(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha3_256_duplicate, .-libspdm_sha3_256_duplicate

    .globl libspdm_sha3_256_final
    .type libspdm_sha3_256_final, @function
libspdm_sha3_256_final:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_256_final(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha3_256_final, .-libspdm_sha3_256_final

    .globl libspdm_sha3_256_free
    .type libspdm_sha3_256_free, @function
libspdm_sha3_256_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_256_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_sha3_256_free, .-libspdm_sha3_256_free

    .globl libspdm_sha3_256_hash_all
    .type libspdm_sha3_256_hash_all, @function
libspdm_sha3_256_hash_all:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_256_hash_all(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha3_256_hash_all, .-libspdm_sha3_256_hash_all

    .globl libspdm_sha3_256_init
    .type libspdm_sha3_256_init, @function
libspdm_sha3_256_init:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_256_init(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha3_256_init, .-libspdm_sha3_256_init

    .globl libspdm_sha3_256_new
    .type libspdm_sha3_256_new, @function
libspdm_sha3_256_new:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_256_new(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha3_256_new, .-libspdm_sha3_256_new

    .globl libspdm_sha3_256_update
    .type libspdm_sha3_256_update, @function
libspdm_sha3_256_update:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_256_update(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha3_256_update, .-libspdm_sha3_256_update

    .globl libspdm_sha3_384_duplicate
    .type libspdm_sha3_384_duplicate, @function
libspdm_sha3_384_duplicate:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_384_duplicate(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha3_384_duplicate, .-libspdm_sha3_384_duplicate

    .globl libspdm_sha3_384_final
    .type libspdm_sha3_384_final, @function
libspdm_sha3_384_final:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_384_final(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha3_384_final, .-libspdm_sha3_384_final

    .globl libspdm_sha3_384_free
    .type libspdm_sha3_384_free, @function
libspdm_sha3_384_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_384_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_sha3_384_free, .-libspdm_sha3_384_free

    .globl libspdm_sha3_384_hash_all
    .type libspdm_sha3_384_hash_all, @function
libspdm_sha3_384_hash_all:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_384_hash_all(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha3_384_hash_all, .-libspdm_sha3_384_hash_all

    .globl libspdm_sha3_384_init
    .type libspdm_sha3_384_init, @function
libspdm_sha3_384_init:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_384_init(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha3_384_init, .-libspdm_sha3_384_init

    .globl libspdm_sha3_384_new
    .type libspdm_sha3_384_new, @function
libspdm_sha3_384_new:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_384_new(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha3_384_new, .-libspdm_sha3_384_new

    .globl libspdm_sha3_384_update
    .type libspdm_sha3_384_update, @function
libspdm_sha3_384_update:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_384_update(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha3_384_update, .-libspdm_sha3_384_update

    .globl libspdm_sha3_512_duplicate
    .type libspdm_sha3_512_duplicate, @function
libspdm_sha3_512_duplicate:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_512_duplicate(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha3_512_duplicate, .-libspdm_sha3_512_duplicate

    .globl libspdm_sha3_512_final
    .type libspdm_sha3_512_final, @function
libspdm_sha3_512_final:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_512_final(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha3_512_final, .-libspdm_sha3_512_final

    .globl libspdm_sha3_512_free
    .type libspdm_sha3_512_free, @function
libspdm_sha3_512_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_512_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_sha3_512_free, .-libspdm_sha3_512_free

    .globl libspdm_sha3_512_hash_all
    .type libspdm_sha3_512_hash_all, @function
libspdm_sha3_512_hash_all:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_512_hash_all(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha3_512_hash_all, .-libspdm_sha3_512_hash_all

    .globl libspdm_sha3_512_init
    .type libspdm_sha3_512_init, @function
libspdm_sha3_512_init:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_512_init(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha3_512_init, .-libspdm_sha3_512_init

    .globl libspdm_sha3_512_new
    .type libspdm_sha3_512_new, @function
libspdm_sha3_512_new:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_512_new(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha3_512_new, .-libspdm_sha3_512_new

    .globl libspdm_sha3_512_update
    .type libspdm_sha3_512_update, @function
libspdm_sha3_512_update:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha3_512_update(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha3_512_update, .-libspdm_sha3_512_update

    .globl libspdm_sha384_duplicate
    .type libspdm_sha384_duplicate, @function
libspdm_sha384_duplicate:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha384_duplicate(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha384_duplicate, .-libspdm_sha384_duplicate

    .globl libspdm_sha384_final
    .type libspdm_sha384_final, @function
libspdm_sha384_final:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha384_final(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha384_final, .-libspdm_sha384_final

    .globl libspdm_sha384_free
    .type libspdm_sha384_free, @function
libspdm_sha384_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha384_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_sha384_free, .-libspdm_sha384_free

    .globl libspdm_sha384_hash_all
    .type libspdm_sha384_hash_all, @function
libspdm_sha384_hash_all:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha384_hash_all(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha384_hash_all, .-libspdm_sha384_hash_all

    .globl libspdm_sha384_init
    .type libspdm_sha384_init, @function
libspdm_sha384_init:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha384_init(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha384_init, .-libspdm_sha384_init

    .globl libspdm_sha384_new
    .type libspdm_sha384_new, @function
libspdm_sha384_new:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha384_new(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha384_new, .-libspdm_sha384_new

    .globl libspdm_sha384_update
    .type libspdm_sha384_update, @function
libspdm_sha384_update:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha384_update(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha384_update, .-libspdm_sha384_update

    .globl libspdm_sha512_duplicate
    .type libspdm_sha512_duplicate, @function
libspdm_sha512_duplicate:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha512_duplicate(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha512_duplicate, .-libspdm_sha512_duplicate

    .globl libspdm_sha512_final
    .type libspdm_sha512_final, @function
libspdm_sha512_final:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha512_final(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha512_final, .-libspdm_sha512_final

    .globl libspdm_sha512_free
    .type libspdm_sha512_free, @function
libspdm_sha512_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha512_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_sha512_free, .-libspdm_sha512_free

    .globl libspdm_sha512_hash_all
    .type libspdm_sha512_hash_all, @function
libspdm_sha512_hash_all:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha512_hash_all(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha512_hash_all, .-libspdm_sha512_hash_all

    .globl libspdm_sha512_init
    .type libspdm_sha512_init, @function
libspdm_sha512_init:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha512_init(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha512_init, .-libspdm_sha512_init

    .globl libspdm_sha512_new
    .type libspdm_sha512_new, @function
libspdm_sha512_new:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha512_new(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha512_new, .-libspdm_sha512_new

    .globl libspdm_sha512_update
    .type libspdm_sha512_update, @function
libspdm_sha512_update:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sha512_update(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sha512_update, .-libspdm_sha512_update

    .globl libspdm_sm2_dsa_free
    .type libspdm_sm2_dsa_free, @function
libspdm_sm2_dsa_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sm2_dsa_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_sm2_dsa_free, .-libspdm_sm2_dsa_free

    .globl libspdm_sm2_dsa_sign
    .type libspdm_sm2_dsa_sign, @function
libspdm_sm2_dsa_sign:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sm2_dsa_sign(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sm2_dsa_sign, .-libspdm_sm2_dsa_sign

    .globl libspdm_sm2_dsa_verify
    .type libspdm_sm2_dsa_verify, @function
libspdm_sm2_dsa_verify:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sm2_dsa_verify(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sm2_dsa_verify, .-libspdm_sm2_dsa_verify

    .globl libspdm_sm2_get_public_key_from_der
    .type libspdm_sm2_get_public_key_from_der, @function
libspdm_sm2_get_public_key_from_der:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sm2_get_public_key_from_der(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sm2_get_public_key_from_der, .-libspdm_sm2_get_public_key_from_der

    .globl libspdm_sm2_get_public_key_from_x509
    .type libspdm_sm2_get_public_key_from_x509, @function
libspdm_sm2_get_public_key_from_x509:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sm2_get_public_key_from_x509(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sm2_get_public_key_from_x509, .-libspdm_sm2_get_public_key_from_x509

    .globl libspdm_sm2_key_exchange_compute_key
    .type libspdm_sm2_key_exchange_compute_key, @function
libspdm_sm2_key_exchange_compute_key:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sm2_key_exchange_compute_key(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sm2_key_exchange_compute_key, .-libspdm_sm2_key_exchange_compute_key

    .globl libspdm_sm2_key_exchange_free
    .type libspdm_sm2_key_exchange_free, @function
libspdm_sm2_key_exchange_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sm2_key_exchange_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_sm2_key_exchange_free, .-libspdm_sm2_key_exchange_free

    .globl libspdm_sm2_key_exchange_generate_key
    .type libspdm_sm2_key_exchange_generate_key, @function
libspdm_sm2_key_exchange_generate_key:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sm2_key_exchange_generate_key(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sm2_key_exchange_generate_key, .-libspdm_sm2_key_exchange_generate_key

    .globl libspdm_sm2_key_exchange_init
    .type libspdm_sm2_key_exchange_init, @function
libspdm_sm2_key_exchange_init:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sm2_key_exchange_init(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sm2_key_exchange_init, .-libspdm_sm2_key_exchange_init

    .globl libspdm_sm2_key_exchange_new_by_nid
    .type libspdm_sm2_key_exchange_new_by_nid, @function
libspdm_sm2_key_exchange_new_by_nid:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sm2_key_exchange_new_by_nid(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sm2_key_exchange_new_by_nid, .-libspdm_sm2_key_exchange_new_by_nid

    .globl libspdm_sm3_256_duplicate
    .type libspdm_sm3_256_duplicate, @function
libspdm_sm3_256_duplicate:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sm3_256_duplicate(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sm3_256_duplicate, .-libspdm_sm3_256_duplicate

    .globl libspdm_sm3_256_final
    .type libspdm_sm3_256_final, @function
libspdm_sm3_256_final:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sm3_256_final(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sm3_256_final, .-libspdm_sm3_256_final

    .globl libspdm_sm3_256_free
    .type libspdm_sm3_256_free, @function
libspdm_sm3_256_free:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sm3_256_free(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    # void 返回
    popq %rbp
    ret
    .size libspdm_sm3_256_free, .-libspdm_sm3_256_free

    .globl libspdm_sm3_256_hash_all
    .type libspdm_sm3_256_hash_all, @function
libspdm_sm3_256_hash_all:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sm3_256_hash_all(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sm3_256_hash_all, .-libspdm_sm3_256_hash_all

    .globl libspdm_sm3_256_init
    .type libspdm_sm3_256_init, @function
libspdm_sm3_256_init:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sm3_256_init(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sm3_256_init, .-libspdm_sm3_256_init

    .globl libspdm_sm3_256_new
    .type libspdm_sm3_256_new, @function
libspdm_sm3_256_new:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sm3_256_new(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sm3_256_new, .-libspdm_sm3_256_new

    .globl libspdm_sm3_256_update
    .type libspdm_sm3_256_update, @function
libspdm_sm3_256_update:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_sm3_256_update(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_sm3_256_update, .-libspdm_sm3_256_update

    .globl libspdm_x509_compare_date_time
    .type libspdm_x509_compare_date_time, @function
libspdm_x509_compare_date_time:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_x509_compare_date_time(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    movl $-38, %eax
    popq %rbp
    ret
    .size libspdm_x509_compare_date_time, .-libspdm_x509_compare_date_time

    .globl libspdm_x509_get_cert_from_cert_chain
    .type libspdm_x509_get_cert_from_cert_chain, @function
libspdm_x509_get_cert_from_cert_chain:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_x509_get_cert_from_cert_chain(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_x509_get_cert_from_cert_chain, .-libspdm_x509_get_cert_from_cert_chain

    .globl libspdm_x509_get_extended_basic_constraints
    .type libspdm_x509_get_extended_basic_constraints, @function
libspdm_x509_get_extended_basic_constraints:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_x509_get_extended_basic_constraints(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_x509_get_extended_basic_constraints, .-libspdm_x509_get_extended_basic_constraints

    .globl libspdm_x509_get_extended_key_usage
    .type libspdm_x509_get_extended_key_usage, @function
libspdm_x509_get_extended_key_usage:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_x509_get_extended_key_usage(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_x509_get_extended_key_usage, .-libspdm_x509_get_extended_key_usage

    .globl libspdm_x509_get_extension_data
    .type libspdm_x509_get_extension_data, @function
libspdm_x509_get_extension_data:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_x509_get_extension_data(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_x509_get_extension_data, .-libspdm_x509_get_extension_data

    .globl libspdm_x509_get_issuer_name
    .type libspdm_x509_get_issuer_name, @function
libspdm_x509_get_issuer_name:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_x509_get_issuer_name(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_x509_get_issuer_name, .-libspdm_x509_get_issuer_name

    .globl libspdm_x509_get_key_usage
    .type libspdm_x509_get_key_usage, @function
libspdm_x509_get_key_usage:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_x509_get_key_usage(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_x509_get_key_usage, .-libspdm_x509_get_key_usage

    .globl libspdm_x509_get_serial_number
    .type libspdm_x509_get_serial_number, @function
libspdm_x509_get_serial_number:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_x509_get_serial_number(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_x509_get_serial_number, .-libspdm_x509_get_serial_number

    .globl libspdm_x509_get_subject_name
    .type libspdm_x509_get_subject_name, @function
libspdm_x509_get_subject_name:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_x509_get_subject_name(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_x509_get_subject_name, .-libspdm_x509_get_subject_name

    .globl libspdm_x509_get_validity
    .type libspdm_x509_get_validity, @function
libspdm_x509_get_validity:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_x509_get_validity(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_x509_get_validity, .-libspdm_x509_get_validity

    .globl libspdm_x509_get_version
    .type libspdm_x509_get_version, @function
libspdm_x509_get_version:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_x509_get_version(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_x509_get_version, .-libspdm_x509_get_version

    .globl libspdm_x509_set_date_time
    .type libspdm_x509_set_date_time, @function
libspdm_x509_set_date_time:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_x509_set_date_time(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_x509_set_date_time, .-libspdm_x509_set_date_time

    .globl libspdm_x509_verify_cert
    .type libspdm_x509_verify_cert, @function
libspdm_x509_verify_cert:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_x509_verify_cert(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_x509_verify_cert, .-libspdm_x509_verify_cert

    .globl libspdm_x509_verify_cert_chain
    .type libspdm_x509_verify_cert_chain, @function
libspdm_x509_verify_cert_chain:
    pushq %rbp
    movq %rsp, %rbp
    leaq .Lnvabi_msg_libspdm_x509_verify_cert_chain(%rip), %rdi
    leaq .Lnvabi_stage_libspdm(%rip), %rsi
    call nvabi_log_unsupported
    xorl %eax, %eax
    popq %rbp
    ret
    .size libspdm_x509_verify_cert_chain, .-libspdm_x509_verify_cert_chain

    .section .rodata.nvabi_libspdm_unsupported,"a",@progbits
.Lnvabi_stage_libspdm:
    .asciz "vendor-abi/libspdm-unsupported"
.Lnvabi_msg_libspdm_aead_aes_gcm_decrypt:
    .asciz "libspdm_aead_aes_gcm_decrypt"
.Lnvabi_msg_libspdm_aead_aes_gcm_decrypt_prealloc:
    .asciz "libspdm_aead_aes_gcm_decrypt_prealloc"
.Lnvabi_msg_libspdm_aead_aes_gcm_encrypt:
    .asciz "libspdm_aead_aes_gcm_encrypt"
.Lnvabi_msg_libspdm_aead_aes_gcm_encrypt_prealloc:
    .asciz "libspdm_aead_aes_gcm_encrypt_prealloc"
.Lnvabi_msg_libspdm_aead_chacha20_poly1305_decrypt:
    .asciz "libspdm_aead_chacha20_poly1305_decrypt"
.Lnvabi_msg_libspdm_aead_chacha20_poly1305_encrypt:
    .asciz "libspdm_aead_chacha20_poly1305_encrypt"
.Lnvabi_msg_libspdm_aead_free:
    .asciz "libspdm_aead_free"
.Lnvabi_msg_libspdm_aead_gcm_prealloc:
    .asciz "libspdm_aead_gcm_prealloc"
.Lnvabi_msg_libspdm_aead_sm4_gcm_decrypt:
    .asciz "libspdm_aead_sm4_gcm_decrypt"
.Lnvabi_msg_libspdm_aead_sm4_gcm_encrypt:
    .asciz "libspdm_aead_sm4_gcm_encrypt"
.Lnvabi_msg_libspdm_asn1_get_tag:
    .asciz "libspdm_asn1_get_tag"
.Lnvabi_msg_libspdm_check_crypto_backend:
    .asciz "libspdm_check_crypto_backend"
.Lnvabi_msg_libspdm_decode_base64:
    .asciz "libspdm_decode_base64"
.Lnvabi_msg_libspdm_dh_compute_key:
    .asciz "libspdm_dh_compute_key"
.Lnvabi_msg_libspdm_dh_free:
    .asciz "libspdm_dh_free"
.Lnvabi_msg_libspdm_dh_generate_key:
    .asciz "libspdm_dh_generate_key"
.Lnvabi_msg_libspdm_dh_new_by_nid:
    .asciz "libspdm_dh_new_by_nid"
.Lnvabi_msg_libspdm_ec_compute_key:
    .asciz "libspdm_ec_compute_key"
.Lnvabi_msg_libspdm_ec_free:
    .asciz "libspdm_ec_free"
.Lnvabi_msg_libspdm_ec_generate_key:
    .asciz "libspdm_ec_generate_key"
.Lnvabi_msg_libspdm_ec_get_public_key_from_x509:
    .asciz "libspdm_ec_get_public_key_from_x509"
.Lnvabi_msg_libspdm_ec_new_by_nid:
    .asciz "libspdm_ec_new_by_nid"
.Lnvabi_msg_libspdm_ecd_free:
    .asciz "libspdm_ecd_free"
.Lnvabi_msg_libspdm_ecd_get_public_key_from_der:
    .asciz "libspdm_ecd_get_public_key_from_der"
.Lnvabi_msg_libspdm_ecd_get_public_key_from_x509:
    .asciz "libspdm_ecd_get_public_key_from_x509"
.Lnvabi_msg_libspdm_ecdsa_sign:
    .asciz "libspdm_ecdsa_sign"
.Lnvabi_msg_libspdm_ecdsa_verify:
    .asciz "libspdm_ecdsa_verify"
.Lnvabi_msg_libspdm_eddsa_sign:
    .asciz "libspdm_eddsa_sign"
.Lnvabi_msg_libspdm_eddsa_verify:
    .asciz "libspdm_eddsa_verify"
.Lnvabi_msg_libspdm_encode_base64:
    .asciz "libspdm_encode_base64"
.Lnvabi_msg_libspdm_get_encap_response_challenge_auth:
    .asciz "libspdm_get_encap_response_challenge_auth"
.Lnvabi_msg_libspdm_hkdf_sha256_expand:
    .asciz "libspdm_hkdf_sha256_expand"
.Lnvabi_msg_libspdm_hkdf_sha256_extract:
    .asciz "libspdm_hkdf_sha256_extract"
.Lnvabi_msg_libspdm_hkdf_sha3_256_expand:
    .asciz "libspdm_hkdf_sha3_256_expand"
.Lnvabi_msg_libspdm_hkdf_sha3_256_extract:
    .asciz "libspdm_hkdf_sha3_256_extract"
.Lnvabi_msg_libspdm_hkdf_sha3_384_expand:
    .asciz "libspdm_hkdf_sha3_384_expand"
.Lnvabi_msg_libspdm_hkdf_sha3_384_extract:
    .asciz "libspdm_hkdf_sha3_384_extract"
.Lnvabi_msg_libspdm_hkdf_sha3_512_expand:
    .asciz "libspdm_hkdf_sha3_512_expand"
.Lnvabi_msg_libspdm_hkdf_sha3_512_extract:
    .asciz "libspdm_hkdf_sha3_512_extract"
.Lnvabi_msg_libspdm_hkdf_sha384_expand:
    .asciz "libspdm_hkdf_sha384_expand"
.Lnvabi_msg_libspdm_hkdf_sha384_extract:
    .asciz "libspdm_hkdf_sha384_extract"
.Lnvabi_msg_libspdm_hkdf_sha512_expand:
    .asciz "libspdm_hkdf_sha512_expand"
.Lnvabi_msg_libspdm_hkdf_sha512_extract:
    .asciz "libspdm_hkdf_sha512_extract"
.Lnvabi_msg_libspdm_hkdf_sm3_256_expand:
    .asciz "libspdm_hkdf_sm3_256_expand"
.Lnvabi_msg_libspdm_hkdf_sm3_256_extract:
    .asciz "libspdm_hkdf_sm3_256_extract"
.Lnvabi_msg_libspdm_hmac_sha256_all:
    .asciz "libspdm_hmac_sha256_all"
.Lnvabi_msg_libspdm_hmac_sha256_duplicate:
    .asciz "libspdm_hmac_sha256_duplicate"
.Lnvabi_msg_libspdm_hmac_sha256_final:
    .asciz "libspdm_hmac_sha256_final"
.Lnvabi_msg_libspdm_hmac_sha256_free:
    .asciz "libspdm_hmac_sha256_free"
.Lnvabi_msg_libspdm_hmac_sha256_new:
    .asciz "libspdm_hmac_sha256_new"
.Lnvabi_msg_libspdm_hmac_sha256_set_key:
    .asciz "libspdm_hmac_sha256_set_key"
.Lnvabi_msg_libspdm_hmac_sha256_update:
    .asciz "libspdm_hmac_sha256_update"
.Lnvabi_msg_libspdm_hmac_sha3_256_all:
    .asciz "libspdm_hmac_sha3_256_all"
.Lnvabi_msg_libspdm_hmac_sha3_256_duplicate:
    .asciz "libspdm_hmac_sha3_256_duplicate"
.Lnvabi_msg_libspdm_hmac_sha3_256_final:
    .asciz "libspdm_hmac_sha3_256_final"
.Lnvabi_msg_libspdm_hmac_sha3_256_free:
    .asciz "libspdm_hmac_sha3_256_free"
.Lnvabi_msg_libspdm_hmac_sha3_256_new:
    .asciz "libspdm_hmac_sha3_256_new"
.Lnvabi_msg_libspdm_hmac_sha3_256_set_key:
    .asciz "libspdm_hmac_sha3_256_set_key"
.Lnvabi_msg_libspdm_hmac_sha3_256_update:
    .asciz "libspdm_hmac_sha3_256_update"
.Lnvabi_msg_libspdm_hmac_sha3_384_all:
    .asciz "libspdm_hmac_sha3_384_all"
.Lnvabi_msg_libspdm_hmac_sha3_384_duplicate:
    .asciz "libspdm_hmac_sha3_384_duplicate"
.Lnvabi_msg_libspdm_hmac_sha3_384_final:
    .asciz "libspdm_hmac_sha3_384_final"
.Lnvabi_msg_libspdm_hmac_sha3_384_free:
    .asciz "libspdm_hmac_sha3_384_free"
.Lnvabi_msg_libspdm_hmac_sha3_384_new:
    .asciz "libspdm_hmac_sha3_384_new"
.Lnvabi_msg_libspdm_hmac_sha3_384_set_key:
    .asciz "libspdm_hmac_sha3_384_set_key"
.Lnvabi_msg_libspdm_hmac_sha3_384_update:
    .asciz "libspdm_hmac_sha3_384_update"
.Lnvabi_msg_libspdm_hmac_sha3_512_all:
    .asciz "libspdm_hmac_sha3_512_all"
.Lnvabi_msg_libspdm_hmac_sha3_512_duplicate:
    .asciz "libspdm_hmac_sha3_512_duplicate"
.Lnvabi_msg_libspdm_hmac_sha3_512_final:
    .asciz "libspdm_hmac_sha3_512_final"
.Lnvabi_msg_libspdm_hmac_sha3_512_free:
    .asciz "libspdm_hmac_sha3_512_free"
.Lnvabi_msg_libspdm_hmac_sha3_512_new:
    .asciz "libspdm_hmac_sha3_512_new"
.Lnvabi_msg_libspdm_hmac_sha3_512_set_key:
    .asciz "libspdm_hmac_sha3_512_set_key"
.Lnvabi_msg_libspdm_hmac_sha3_512_update:
    .asciz "libspdm_hmac_sha3_512_update"
.Lnvabi_msg_libspdm_hmac_sha384_all:
    .asciz "libspdm_hmac_sha384_all"
.Lnvabi_msg_libspdm_hmac_sha384_duplicate:
    .asciz "libspdm_hmac_sha384_duplicate"
.Lnvabi_msg_libspdm_hmac_sha384_final:
    .asciz "libspdm_hmac_sha384_final"
.Lnvabi_msg_libspdm_hmac_sha384_free:
    .asciz "libspdm_hmac_sha384_free"
.Lnvabi_msg_libspdm_hmac_sha384_new:
    .asciz "libspdm_hmac_sha384_new"
.Lnvabi_msg_libspdm_hmac_sha384_set_key:
    .asciz "libspdm_hmac_sha384_set_key"
.Lnvabi_msg_libspdm_hmac_sha384_update:
    .asciz "libspdm_hmac_sha384_update"
.Lnvabi_msg_libspdm_hmac_sha512_all:
    .asciz "libspdm_hmac_sha512_all"
.Lnvabi_msg_libspdm_hmac_sha512_duplicate:
    .asciz "libspdm_hmac_sha512_duplicate"
.Lnvabi_msg_libspdm_hmac_sha512_final:
    .asciz "libspdm_hmac_sha512_final"
.Lnvabi_msg_libspdm_hmac_sha512_free:
    .asciz "libspdm_hmac_sha512_free"
.Lnvabi_msg_libspdm_hmac_sha512_new:
    .asciz "libspdm_hmac_sha512_new"
.Lnvabi_msg_libspdm_hmac_sha512_set_key:
    .asciz "libspdm_hmac_sha512_set_key"
.Lnvabi_msg_libspdm_hmac_sha512_update:
    .asciz "libspdm_hmac_sha512_update"
.Lnvabi_msg_libspdm_hmac_sm3_256_all:
    .asciz "libspdm_hmac_sm3_256_all"
.Lnvabi_msg_libspdm_hmac_sm3_256_duplicate:
    .asciz "libspdm_hmac_sm3_256_duplicate"
.Lnvabi_msg_libspdm_hmac_sm3_256_final:
    .asciz "libspdm_hmac_sm3_256_final"
.Lnvabi_msg_libspdm_hmac_sm3_256_free:
    .asciz "libspdm_hmac_sm3_256_free"
.Lnvabi_msg_libspdm_hmac_sm3_256_new:
    .asciz "libspdm_hmac_sm3_256_new"
.Lnvabi_msg_libspdm_hmac_sm3_256_set_key:
    .asciz "libspdm_hmac_sm3_256_set_key"
.Lnvabi_msg_libspdm_hmac_sm3_256_update:
    .asciz "libspdm_hmac_sm3_256_update"
.Lnvabi_msg_libspdm_random_bytes:
    .asciz "libspdm_random_bytes"
.Lnvabi_msg_libspdm_rsa_free:
    .asciz "libspdm_rsa_free"
.Lnvabi_msg_libspdm_rsa_get_public_key_from_x509:
    .asciz "libspdm_rsa_get_public_key_from_x509"
.Lnvabi_msg_libspdm_rsa_new:
    .asciz "libspdm_rsa_new"
.Lnvabi_msg_libspdm_rsa_pkcs1_sign_with_nid:
    .asciz "libspdm_rsa_pkcs1_sign_with_nid"
.Lnvabi_msg_libspdm_rsa_pkcs1_verify_with_nid:
    .asciz "libspdm_rsa_pkcs1_verify_with_nid"
.Lnvabi_msg_libspdm_rsa_pss_sign:
    .asciz "libspdm_rsa_pss_sign"
.Lnvabi_msg_libspdm_rsa_pss_verify:
    .asciz "libspdm_rsa_pss_verify"
.Lnvabi_msg_libspdm_rsa_set_key:
    .asciz "libspdm_rsa_set_key"
.Lnvabi_msg_libspdm_send_receive_psk_exchange:
    .asciz "libspdm_send_receive_psk_exchange"
.Lnvabi_msg_libspdm_send_receive_psk_exchange_ex:
    .asciz "libspdm_send_receive_psk_exchange_ex"
.Lnvabi_msg_libspdm_send_receive_psk_finish:
    .asciz "libspdm_send_receive_psk_finish"
.Lnvabi_msg_libspdm_sha256_duplicate:
    .asciz "libspdm_sha256_duplicate"
.Lnvabi_msg_libspdm_sha256_final:
    .asciz "libspdm_sha256_final"
.Lnvabi_msg_libspdm_sha256_free:
    .asciz "libspdm_sha256_free"
.Lnvabi_msg_libspdm_sha256_hash_all:
    .asciz "libspdm_sha256_hash_all"
.Lnvabi_msg_libspdm_sha256_init:
    .asciz "libspdm_sha256_init"
.Lnvabi_msg_libspdm_sha256_new:
    .asciz "libspdm_sha256_new"
.Lnvabi_msg_libspdm_sha256_update:
    .asciz "libspdm_sha256_update"
.Lnvabi_msg_libspdm_sha3_256_duplicate:
    .asciz "libspdm_sha3_256_duplicate"
.Lnvabi_msg_libspdm_sha3_256_final:
    .asciz "libspdm_sha3_256_final"
.Lnvabi_msg_libspdm_sha3_256_free:
    .asciz "libspdm_sha3_256_free"
.Lnvabi_msg_libspdm_sha3_256_hash_all:
    .asciz "libspdm_sha3_256_hash_all"
.Lnvabi_msg_libspdm_sha3_256_init:
    .asciz "libspdm_sha3_256_init"
.Lnvabi_msg_libspdm_sha3_256_new:
    .asciz "libspdm_sha3_256_new"
.Lnvabi_msg_libspdm_sha3_256_update:
    .asciz "libspdm_sha3_256_update"
.Lnvabi_msg_libspdm_sha3_384_duplicate:
    .asciz "libspdm_sha3_384_duplicate"
.Lnvabi_msg_libspdm_sha3_384_final:
    .asciz "libspdm_sha3_384_final"
.Lnvabi_msg_libspdm_sha3_384_free:
    .asciz "libspdm_sha3_384_free"
.Lnvabi_msg_libspdm_sha3_384_hash_all:
    .asciz "libspdm_sha3_384_hash_all"
.Lnvabi_msg_libspdm_sha3_384_init:
    .asciz "libspdm_sha3_384_init"
.Lnvabi_msg_libspdm_sha3_384_new:
    .asciz "libspdm_sha3_384_new"
.Lnvabi_msg_libspdm_sha3_384_update:
    .asciz "libspdm_sha3_384_update"
.Lnvabi_msg_libspdm_sha3_512_duplicate:
    .asciz "libspdm_sha3_512_duplicate"
.Lnvabi_msg_libspdm_sha3_512_final:
    .asciz "libspdm_sha3_512_final"
.Lnvabi_msg_libspdm_sha3_512_free:
    .asciz "libspdm_sha3_512_free"
.Lnvabi_msg_libspdm_sha3_512_hash_all:
    .asciz "libspdm_sha3_512_hash_all"
.Lnvabi_msg_libspdm_sha3_512_init:
    .asciz "libspdm_sha3_512_init"
.Lnvabi_msg_libspdm_sha3_512_new:
    .asciz "libspdm_sha3_512_new"
.Lnvabi_msg_libspdm_sha3_512_update:
    .asciz "libspdm_sha3_512_update"
.Lnvabi_msg_libspdm_sha384_duplicate:
    .asciz "libspdm_sha384_duplicate"
.Lnvabi_msg_libspdm_sha384_final:
    .asciz "libspdm_sha384_final"
.Lnvabi_msg_libspdm_sha384_free:
    .asciz "libspdm_sha384_free"
.Lnvabi_msg_libspdm_sha384_hash_all:
    .asciz "libspdm_sha384_hash_all"
.Lnvabi_msg_libspdm_sha384_init:
    .asciz "libspdm_sha384_init"
.Lnvabi_msg_libspdm_sha384_new:
    .asciz "libspdm_sha384_new"
.Lnvabi_msg_libspdm_sha384_update:
    .asciz "libspdm_sha384_update"
.Lnvabi_msg_libspdm_sha512_duplicate:
    .asciz "libspdm_sha512_duplicate"
.Lnvabi_msg_libspdm_sha512_final:
    .asciz "libspdm_sha512_final"
.Lnvabi_msg_libspdm_sha512_free:
    .asciz "libspdm_sha512_free"
.Lnvabi_msg_libspdm_sha512_hash_all:
    .asciz "libspdm_sha512_hash_all"
.Lnvabi_msg_libspdm_sha512_init:
    .asciz "libspdm_sha512_init"
.Lnvabi_msg_libspdm_sha512_new:
    .asciz "libspdm_sha512_new"
.Lnvabi_msg_libspdm_sha512_update:
    .asciz "libspdm_sha512_update"
.Lnvabi_msg_libspdm_sm2_dsa_free:
    .asciz "libspdm_sm2_dsa_free"
.Lnvabi_msg_libspdm_sm2_dsa_sign:
    .asciz "libspdm_sm2_dsa_sign"
.Lnvabi_msg_libspdm_sm2_dsa_verify:
    .asciz "libspdm_sm2_dsa_verify"
.Lnvabi_msg_libspdm_sm2_get_public_key_from_der:
    .asciz "libspdm_sm2_get_public_key_from_der"
.Lnvabi_msg_libspdm_sm2_get_public_key_from_x509:
    .asciz "libspdm_sm2_get_public_key_from_x509"
.Lnvabi_msg_libspdm_sm2_key_exchange_compute_key:
    .asciz "libspdm_sm2_key_exchange_compute_key"
.Lnvabi_msg_libspdm_sm2_key_exchange_free:
    .asciz "libspdm_sm2_key_exchange_free"
.Lnvabi_msg_libspdm_sm2_key_exchange_generate_key:
    .asciz "libspdm_sm2_key_exchange_generate_key"
.Lnvabi_msg_libspdm_sm2_key_exchange_init:
    .asciz "libspdm_sm2_key_exchange_init"
.Lnvabi_msg_libspdm_sm2_key_exchange_new_by_nid:
    .asciz "libspdm_sm2_key_exchange_new_by_nid"
.Lnvabi_msg_libspdm_sm3_256_duplicate:
    .asciz "libspdm_sm3_256_duplicate"
.Lnvabi_msg_libspdm_sm3_256_final:
    .asciz "libspdm_sm3_256_final"
.Lnvabi_msg_libspdm_sm3_256_free:
    .asciz "libspdm_sm3_256_free"
.Lnvabi_msg_libspdm_sm3_256_hash_all:
    .asciz "libspdm_sm3_256_hash_all"
.Lnvabi_msg_libspdm_sm3_256_init:
    .asciz "libspdm_sm3_256_init"
.Lnvabi_msg_libspdm_sm3_256_new:
    .asciz "libspdm_sm3_256_new"
.Lnvabi_msg_libspdm_sm3_256_update:
    .asciz "libspdm_sm3_256_update"
.Lnvabi_msg_libspdm_x509_compare_date_time:
    .asciz "libspdm_x509_compare_date_time"
.Lnvabi_msg_libspdm_x509_get_cert_from_cert_chain:
    .asciz "libspdm_x509_get_cert_from_cert_chain"
.Lnvabi_msg_libspdm_x509_get_extended_basic_constraints:
    .asciz "libspdm_x509_get_extended_basic_constraints"
.Lnvabi_msg_libspdm_x509_get_extended_key_usage:
    .asciz "libspdm_x509_get_extended_key_usage"
.Lnvabi_msg_libspdm_x509_get_extension_data:
    .asciz "libspdm_x509_get_extension_data"
.Lnvabi_msg_libspdm_x509_get_issuer_name:
    .asciz "libspdm_x509_get_issuer_name"
.Lnvabi_msg_libspdm_x509_get_key_usage:
    .asciz "libspdm_x509_get_key_usage"
.Lnvabi_msg_libspdm_x509_get_serial_number:
    .asciz "libspdm_x509_get_serial_number"
.Lnvabi_msg_libspdm_x509_get_subject_name:
    .asciz "libspdm_x509_get_subject_name"
.Lnvabi_msg_libspdm_x509_get_validity:
    .asciz "libspdm_x509_get_validity"
.Lnvabi_msg_libspdm_x509_get_version:
    .asciz "libspdm_x509_get_version"
.Lnvabi_msg_libspdm_x509_set_date_time:
    .asciz "libspdm_x509_set_date_time"
.Lnvabi_msg_libspdm_x509_verify_cert:
    .asciz "libspdm_x509_verify_cert"
.Lnvabi_msg_libspdm_x509_verify_cert_chain:
    .asciz "libspdm_x509_verify_cert_chain"
