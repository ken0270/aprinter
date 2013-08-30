/*
 * Copyright (c) 2013 Ambroz Bizjak
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

#ifndef AMBROLIB_BINARY_CONTROL_H
#define AMBROLIB_BINARY_CONTROL_H

#include <aprinter/meta/FixedPoint.h>

#include <aprinter/BeginNamespace.h>

struct BinaryControlParams {};

template <typename Params, typename MeasurementInterval, typename ValueFixedType>
class BinaryControl {
public:
    using OutputFixedType = FixedPoint<16, false, -16>;
    
    void init (ValueFixedType target)
    {
        m_target = target;
    }
    
    void setTarget (ValueFixedType target)
    {
        m_target = target;
    }
    
    OutputFixedType addMeasurement (ValueFixedType value)
    {
        return (value < m_target) ? OutputFixedType::maxValue() : OutputFixedType::minValue();
    }
    
private:
    ValueFixedType m_target;
};

#include <aprinter/EndNamespace.h>

#endif