/*
 * Copyright (c) 2016 Ambroz Bizjak
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef APRINTER_TRANSFER_VECTOR_H
#define APRINTER_TRANSFER_VECTOR_H

#include <stddef.h>

#include <aprinter/BeginNamespace.h>

template <typename DataWordType>
struct TransferDescriptor {
    DataWordType *buffer_ptr;
    size_t num_words;
};

template <typename DataWordType>
struct TransferVector {
    TransferDescriptor<DataWordType> const *descriptors;
    int num_descriptors;
};

template <typename DataWordType>
bool CheckTransferVector (TransferVector<DataWordType> vector, size_t num_words)
{
    for (int i = 0; i < vector.num_descriptors; i++) {
        if (vector.descriptors[i].num_words == 0) {
            return false;
        }
        num_words -= vector.descriptors[i].num_words;
    }
    return (num_words == 0);
}

#include <aprinter/EndNamespace.h>

#endif
