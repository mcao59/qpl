/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <algorithm>
#include <array>
#include "../../../common/operation_test.hpp"
#include "ta_ll_common.hpp"
#include "source_provider.hpp"

namespace qpl::test {
constexpr uint32_t offsets_table_size = 30u;

typedef struct qpl_compression_huffman_table qpl_compression_huffman_table;

extern "C" qpl_compression_huffman_table *own_huffman_table_get_compression_table(const qpl_huffman_table_t table);

class triplets_fixture : public JobFixture {
protected:
    void SetUp() override {
        JobFixture::SetUp();

        uint32_t job_size = 0u;
        auto     status   = qpl_get_job_size(GetExecutionPath(), &job_size);
        ASSERT_EQ(QPL_STS_OK, status);

        job_buffer            = new uint8_t[job_size];
        decompression_job_ptr = reinterpret_cast<qpl_job *>(job_buffer);
        status                = qpl_init_job(GetExecutionPath(), decompression_job_ptr);
        ASSERT_EQ(QPL_STS_OK, status);
    }

    void TearDown() override {
        JobFixture::TearDown();

        qpl_fini_job(decompression_job_ptr);
        delete[] job_buffer;
    }

    static std::vector<qpl_huffman_triplet> create_triplets_from_table(qpl_huffman_table_t compression_table_ptr) {
        std::vector<qpl_huffman_triplet> result_triplets(256);

        constexpr uint32_t huffman_code_bit_length = 15u;

        auto *literals_matches_table_ptr =
                     reinterpret_cast<uint32_t *>(own_huffman_table_get_compression_table(compression_table_ptr));

        // Cannot use ASSERT here because this is non-void function
        EXPECT_NE(literals_matches_table_ptr, nullptr) << "Compression table is null";
        if (literals_matches_table_ptr == nullptr) {
            return result_triplets;
        }

        const uint16_t qpl_code_mask = (1u << huffman_code_bit_length) - 1u;

        for (uint16_t i = 0u; i < 256u; i++) {
            result_triplets[i].code        = literals_matches_table_ptr[i] & qpl_code_mask;
            result_triplets[i].code_length = literals_matches_table_ptr[i] >> huffman_code_bit_length;
            result_triplets[i].value       = i;
        }

        return result_triplets;
    }

    template <class source_iterator>
    void build_compression_table(qpl_huffman_table_t compression_table_ptr,
                                 source_iterator begin,
                                 source_iterator end) {
        auto           *source_ptr = &*begin;
        const uint32_t source_size = std::distance(begin, end);
        qpl_histogram  deflate_histogram{};

        auto status = qpl_gather_deflate_statistics(source_ptr,
                                                    source_size,
                                                    &deflate_histogram,
                                                    qpl_default_level,
                                                    GetExecutionPath());

        ASSERT_EQ(status, QPL_STS_OK) << "Failed to gather statistics";

        status = qpl_huffman_table_init_with_histogram(compression_table_ptr, &deflate_histogram);

        ASSERT_EQ(status, QPL_STS_OK) << "Failed to build compression table";
    }

    void run_huffman_only_build_decomp_from_compression_table(std::vector<uint8_t> &source_for_compression_table,
                                                              uint64_t flag_be = 0) {
        // Variables
        uint32_t             seed       = GetSeed();
        uint32_t             max_length = static_cast<uint32_t>(source_for_compression_table.size());
        std::vector<uint8_t> reference_buffer;
        uint32_t             status     = QPL_STS_OK;

        qpl_huffman_table_t c_huffman_table;
        qpl_huffman_table_t d_huffman_table;

        status = qpl_huffman_only_table_create(compression_table_type,
                                               GetExecutionPath(),
                                               DEFAULT_ALLOCATOR_C,
                                               &c_huffman_table);
        ASSERT_EQ(status, QPL_STS_OK) << "Table creation failed";

        status = qpl_huffman_only_table_create(decompression_table_type,
                                               GetExecutionPath(),
                                               DEFAULT_ALLOCATOR_C,
                                               &d_huffman_table);
        ASSERT_EQ(status, QPL_STS_OK) << "Table creation failed";

        destination.resize(max_length * 2);
        std::fill(destination.begin(), destination.end(), 0u);

        // Workaround for "no headers" issue (+7) - usually a customer doesn't know the decomressed size;
        // This also should be mentioned in the manual: decompression can generate up to 7 extra
        // bytes from the last byte padding bits.
        reference_buffer.resize(max_length + 7u);
        std::fill(reference_buffer.begin(), reference_buffer.end(), 0u);

        build_compression_table(c_huffman_table,
                                source_for_compression_table.begin(),
                                source_for_compression_table.end());

        auto triplets = create_triplets_from_table(c_huffman_table);

        qpl_huffman_table_t c_triplets_huffman_table;

        status = qpl_huffman_only_table_create(compression_table_type,
                                               GetExecutionPath(),
                                               DEFAULT_ALLOCATOR_C,
                                               &c_triplets_huffman_table);
        ASSERT_EQ(status, QPL_STS_OK) << "Table creation failed";

        status = qpl_huffman_table_init_with_triplets(c_triplets_huffman_table, triplets.data(), triplets.size());

        ASSERT_EQ(QPL_STS_OK, status);

        // Now initialize compression job
        status = qpl_init_job(GetExecutionPath(), job_ptr);
        ASSERT_EQ(QPL_STS_OK, status);

        job_ptr->op            = qpl_op_compress;
        job_ptr->next_in_ptr   = source_for_compression_table.data();
        job_ptr->next_out_ptr  = destination.data();
        job_ptr->available_in  = max_length;
        job_ptr->available_out = max_length * 2;

        job_ptr->huffman_table = c_triplets_huffman_table;
        job_ptr->flags         = QPL_FLAG_FIRST |
                                 QPL_FLAG_LAST |
                                 QPL_FLAG_NO_HDRS |
                                 QPL_FLAG_OMIT_VERIFY |
                                 QPL_FLAG_GEN_LITERALS |
                                 flag_be;

        status = run_job_api(job_ptr);

        if (qpl_path_software == job_ptr->data_ptr.path) {
            ASSERT_EQ(QPL_STS_OK, status);
        } else {
            if (QPL_STS_OK != status) {
                if (QPL_STS_INTL_VERIFY_ERR == status && QPL_FLAG_HUFFMAN_BE == flag_be) {
                    // Fix for HW issue in IAA 1.0 NO_HDR mode does not work for the case of BE16 compression.
                    // This is because we need to drop up to 15 bits, but we can only drop at most 7 bits.
                    // So in some cases, verify will fail in the BE16 case.
                    // The fix for this is too complicated, and this is really a niche operation.
                    // The recommendation is that we dont fix this, and add documentation that in this case
                    // (no headers and big-endian-16), verify not be used, and if the user wants to verify
                    // the output, they should (within the app) decompress the compressed buffer into a new
                    // buffer, and compare that against the original input.

                    std::cout << "Deflate verify stage failed with status: " << " " << status << "\n";
                    std::cout << "It is known issue for Huffman-only with BE16 format with IAA 1.0 - ignoring\n";
                } else {
                    FAIL() << "Deflate status: " << status << "\n";
                }
            }
        }

        decompression_job_ptr->op          = qpl_op_decompress;
        decompression_job_ptr->next_in_ptr = destination.data();

        decompression_job_ptr->next_out_ptr  = reference_buffer.data();
        decompression_job_ptr->available_in  = job_ptr->total_out;
        decompression_job_ptr->available_out = max_length + 7u;
        decompression_job_ptr->huffman_table = d_huffman_table;
        decompression_job_ptr->flags         = QPL_FLAG_NO_HDRS | flag_be;
        decompression_job_ptr->flags |= QPL_FLAG_FIRST | QPL_FLAG_LAST;

        if (QPL_FLAG_HUFFMAN_BE == flag_be) {
            decompression_job_ptr->ignore_end_bits = (16 - job_ptr->last_bit_offset) & 15;
        } else {
            decompression_job_ptr->ignore_end_bits = (8 - job_ptr->last_bit_offset) & 7;
        }

        // Decompress
        status = qpl_huffman_table_init_with_other(d_huffman_table, c_huffman_table);
        ASSERT_EQ(QPL_STS_OK, status);

        // Decompress
        status = run_job_api(decompression_job_ptr);

        // IAA 1.0 limitation: cannot work if ignore_end_bits is greater than 7 bits for BE16 decompress. Expect error in this case.
        bool skip_verify = false;
        if (QPL_FLAG_HUFFMAN_BE == flag_be && qpl_path_hardware == job_ptr->data_ptr.path && decompression_job_ptr->ignore_end_bits > 7) {
            ASSERT_EQ(QPL_STS_HUFFMAN_BE_IGNORE_MORE_THAN_7_BITS_ERR, status);
            skip_verify = true;
        } else {
            ASSERT_EQ(QPL_STS_OK, status);
        }

        // Free resources
        status = qpl_huffman_table_destroy(c_huffman_table);
        ASSERT_EQ(QPL_STS_OK, status);
        status = qpl_huffman_table_destroy(d_huffman_table);
        ASSERT_EQ(QPL_STS_OK, status);
        status = qpl_huffman_table_destroy(c_triplets_huffman_table);
        ASSERT_EQ(QPL_STS_OK, status);

        qpl_fini_job(job_ptr);
        qpl_fini_job(decompression_job_ptr);

        // Verify
        if (!skip_verify) {
            ASSERT_TRUE(CompareVectors(source, reference_buffer, max_length));
        }
    }

    void build_triplets_and_run_huffman_only_deflate(std::vector<uint8_t> &source_for_compression_table,
                                                     uint64_t flag_be = 0) {
        // Variables
        auto                 max_length = static_cast<uint32_t>(source_for_compression_table.size());
        std::vector<uint8_t> reference_buffer;
        uint32_t             status     = QPL_STS_OK;
        qpl_huffman_table_t c_huffman_table;
        qpl_huffman_table_t d_huffman_table;

        status = qpl_huffman_only_table_create(compression_table_type,
                                               GetExecutionPath(),
                                               DEFAULT_ALLOCATOR_C,
                                               &c_huffman_table);
        ASSERT_EQ(status, QPL_STS_OK) << "Table creation failed";

        status = qpl_huffman_only_table_create(decompression_table_type,
                                               GetExecutionPath(),
                                               DEFAULT_ALLOCATOR_C,
                                               &d_huffman_table);
        ASSERT_EQ(status, QPL_STS_OK) << "Table creation failed";

        destination.resize(max_length * 2);
        std::fill(destination.begin(), destination.end(), 0u);

        // Workaround for "no headers" issue (+7) - usually a customer doesn't know the decompressed size;
        // This also should be mentioned in the manual: decompression can generate up to 7 extra
        // bytes from the last byte padding bits.
        reference_buffer.resize(max_length + 7u);
        std::fill(reference_buffer.begin(), reference_buffer.end(), 0u);

        build_compression_table(c_huffman_table,
                                source_for_compression_table.begin(),
                                source_for_compression_table.end());

        auto triplets = create_triplets_from_table(c_huffman_table);

        qpl_huffman_table_t c_triplets_huffman_table;

        status = qpl_huffman_only_table_create(compression_table_type,
                                               GetExecutionPath(),
                                               DEFAULT_ALLOCATOR_C,
                                               &c_triplets_huffman_table);
        ASSERT_EQ(status, QPL_STS_OK) << "Table creation failed";


        status = qpl_huffman_table_init_with_triplets(c_triplets_huffman_table,
                                                      triplets.data(),
                                                      triplets.size());

        ASSERT_EQ(QPL_STS_OK, status);

        // Now initialize compression job
        status = qpl_init_job(GetExecutionPath(), job_ptr);
        ASSERT_EQ(QPL_STS_OK, status);

        job_ptr->op            = qpl_op_compress;
        job_ptr->next_in_ptr   = source_for_compression_table.data();
        job_ptr->next_out_ptr  = destination.data();
        job_ptr->available_in  = max_length;
        job_ptr->available_out = max_length * 2;

        job_ptr->huffman_table = c_triplets_huffman_table;
        job_ptr->flags         = QPL_FLAG_FIRST |
                                 QPL_FLAG_LAST |
                                 QPL_FLAG_NO_HDRS |
                                 QPL_FLAG_OMIT_VERIFY |
                                 QPL_FLAG_GEN_LITERALS |
                                 flag_be;

        status = run_job_api(job_ptr);

        if (qpl_path_software == job_ptr->data_ptr.path) {
            ASSERT_EQ(QPL_STS_OK, status);
        } else {
            if (QPL_STS_OK != status) {
                if (QPL_STS_INTL_VERIFY_ERR == status && QPL_FLAG_HUFFMAN_BE == flag_be) {
                    // Fix for HW issue in IAA 1.0 NO_HDR mode does not work for the case of BE16 compression.
                    // This is because we need to drop up to 15 bits, but we can only drop at most 7 bits.
                    // So in some cases, verify will fail in the BE16 case.
                    // The fix for this is too complicated, and this is really a niche operation.
                    // The recommendation is that we dont fix this, and add documentation that in this case
                    // (no headers and big-endian-16), verify not be used, and if the user wants to verify
                    // the output, they should (within the app) decompress the compressed buffer into a new
                    // buffer, and compare that against the original input.

                    std::cout << "Deflate verify stage failed with status: " << " " << status << "\n";
                    std::cout << "It is known issue for Huffman-only with BE16 format with IAA 1.0 - ignoring\n";
                } else {
                    FAIL() << "Deflate status: " << status << "\n";
                }
            }
        }

        decompression_job_ptr->op          = qpl_op_decompress;
        decompression_job_ptr->next_in_ptr = destination.data();

        decompression_job_ptr->next_out_ptr  = reference_buffer.data();
        decompression_job_ptr->available_in  = job_ptr->total_out;
        decompression_job_ptr->available_out = max_length + 7u;
        decompression_job_ptr->huffman_table = d_huffman_table;
        decompression_job_ptr->flags         = QPL_FLAG_NO_HDRS | flag_be;
        decompression_job_ptr->flags |= QPL_FLAG_FIRST | QPL_FLAG_LAST;

        if (QPL_FLAG_HUFFMAN_BE == flag_be) {
            decompression_job_ptr->ignore_end_bits = (16 - job_ptr->last_bit_offset) & 15;
        } else {
            decompression_job_ptr->ignore_end_bits = (8 - job_ptr->last_bit_offset) & 7;
        }

        // Decompress
        status = qpl_huffman_table_init_with_other(d_huffman_table, c_triplets_huffman_table);

        // Decompress
        status = run_job_api(decompression_job_ptr);

        // IAA 1.0 limitation: cannot work if ignore_end_bits is greater than 7 bits for BE16 decompress. Expect error in this case.
        bool skip_verify = false;
        if (QPL_FLAG_HUFFMAN_BE == flag_be && qpl_path_hardware == job_ptr->data_ptr.path && decompression_job_ptr->ignore_end_bits > 7) {
            ASSERT_EQ(QPL_STS_HUFFMAN_BE_IGNORE_MORE_THAN_7_BITS_ERR, status);
            skip_verify = true;
        } else {
            ASSERT_EQ(QPL_STS_OK, status);
        }

        // Free resources
        status = qpl_huffman_table_destroy(c_huffman_table);
        ASSERT_EQ(QPL_STS_OK, status);
        status = qpl_huffman_table_destroy(d_huffman_table);
        ASSERT_EQ(QPL_STS_OK, status);
        status = qpl_huffman_table_destroy(c_triplets_huffman_table);
        ASSERT_EQ(QPL_STS_OK, status);

        qpl_fini_job(job_ptr);
        qpl_fini_job(decompression_job_ptr);

        // Verify
        if (!skip_verify) {
            ASSERT_TRUE(CompareVectors(source, reference_buffer, max_length));
        }
    }

    uint8_t *job_buffer            = nullptr;
    qpl_job *decompression_job_ptr = nullptr;
};

QPL_LOW_LEVEL_API_ALGORITHMIC_TEST_F(deflate_utility, build_triplets_run_huffman_only_le, triplets_fixture) {
    for (auto &dataset: util::TestEnvironment::GetInstance().GetAlgorithmicDataset().get_data()) {
        source = dataset.second;

        build_triplets_and_run_huffman_only_deflate(source);

        // Clean total_in / out fields before next iteration
        job_ptr->total_in                = 0;
        job_ptr->total_out               = 0;
        decompression_job_ptr->total_in  = 0;
        decompression_job_ptr->total_out = 0;
    }
}

QPL_LOW_LEVEL_API_ALGORITHMIC_TEST_F(deflate_utility, build_triplets_run_huffman_only_be, triplets_fixture) {
    for (auto &dataset: util::TestEnvironment::GetInstance().GetAlgorithmicDataset().get_data()) {
        source = dataset.second;

        build_triplets_and_run_huffman_only_deflate(source, QPL_FLAG_HUFFMAN_BE);

        // Clean total_in / out fields before next iteration
        job_ptr->total_in                = 0;
        job_ptr->total_out               = 0;
        decompression_job_ptr->total_in  = 0;
        decompression_job_ptr->total_out = 0;
    }
}

QPL_FIXTURE_TEST(ta_deflate_utility, run_huffman_only_le_convert_comp_to_decompression_table, triplets_fixture) {
    for (auto &dataset: util::TestEnvironment::GetInstance().GetAlgorithmicDataset().get_data()) {
        source = dataset.second;

        run_huffman_only_build_decomp_from_compression_table(source);

        // Clean total_in / out fields before next iteration
        job_ptr->total_in                = 0;
        job_ptr->total_out               = 0;
        decompression_job_ptr->total_in  = 0;
        decompression_job_ptr->total_out = 0;
    }
}

QPL_FIXTURE_TEST(ta_deflate_utility, run_huffman_only_be_convert_comp_to_decompression_table, triplets_fixture) {
    for (auto &dataset: util::TestEnvironment::GetInstance().GetAlgorithmicDataset().get_data()) {
        source = dataset.second;

        run_huffman_only_build_decomp_from_compression_table(source, QPL_FLAG_HUFFMAN_BE);

        // Clean total_in / out fields before next iteration
        job_ptr->total_in                = 0;
        job_ptr->total_out               = 0;
        decompression_job_ptr->total_in  = 0;
        decompression_job_ptr->total_out = 0;
    }
}

}
