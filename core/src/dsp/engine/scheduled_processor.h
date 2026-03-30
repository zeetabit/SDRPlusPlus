#pragma once
#include "scheduled_block.h"
#include "../processor.h"
#include "../sink.h"
#include "../operator.h"

namespace dsp {

    // Drop-in replacement for dsp::Processor<I,O> that uses the thread pool.
    // Migration: change `class X : public Processor<A,B>` to
    //            `class X : public ScheduledProcessor<A,B>`
    // Everything else (run(), process(), setInput(), macros) stays the same.
    template <class I, class O>
    class ScheduledProcessor : public scheduled_block {
    public:
        ScheduledProcessor() {}

        ScheduledProcessor(stream<I>* in) { init(in); }

        virtual ~ScheduledProcessor() {}

        virtual void init(stream<I>* in) {
            _in = in;
            registerInput(_in);
            registerOutput(&out);
            _block_init = true;
        }

        virtual void setInput(stream<I>* in) {
            assert(_block_init);
            std::lock_guard<std::recursive_mutex> lck(ctrlMtx);
            tempStop();
            unregisterInput(_in);
            _in = in;
            registerInput(_in);
            tempStart();
        }

        virtual int run() = 0;

        stream<O> out;

    protected:
        using base_type = ScheduledProcessor<I, O>;
        stream<I>* _in;
    };

    // Drop-in replacement for dsp::Sink<T> that uses the thread pool.
    template <class T>
    class ScheduledSink : public scheduled_block {
    public:
        ScheduledSink() {}

        ScheduledSink(stream<T>* in) { init(in); }

        virtual ~ScheduledSink() {}

        virtual void init(stream<T>* in) {
            _in = in;
            registerInput(_in);
            _block_init = true;
        }

        virtual void setInput(stream<T>* in) {
            assert(_block_init);
            std::lock_guard<std::recursive_mutex> lck(ctrlMtx);
            tempStop();
            unregisterInput(_in);
            _in = in;
            registerInput(_in);
            tempStart();
        }

        virtual int run() = 0;

    protected:
        stream<T>* _in;
    };

    // Drop-in replacement for dsp::Operator<A,B,O> that uses the thread pool.
    // Migration: change `class X : public Operator<A,B,O>` to
    //            `class X : public ScheduledOperator<A,B,O>`
    template <class A, class B, class O>
    class ScheduledOperator : public scheduled_block {
    public:
        ScheduledOperator() {}

        ScheduledOperator(stream<A>* a, stream<B>* b) { init(a, b); }

        virtual ~ScheduledOperator() {}

        virtual void init(stream<A>* a, stream<B>* b) {
            _a = a;
            _b = b;
            registerInput(_a);
            registerInput(_b);
            registerOutput(&out);
            _block_init = true;
        }

        virtual void setInputs(stream<A>* a, stream<B>* b) {
            assert(_block_init);
            std::lock_guard<std::recursive_mutex> lck(ctrlMtx);
            tempStop();
            unregisterInput(_a);
            unregisterInput(_b);
            _a = a;
            _b = b;
            registerInput(_a);
            registerInput(_b);
            tempStart();
        }

        virtual void setInputA(stream<A>* a) {
            assert(_block_init);
            std::lock_guard<std::recursive_mutex> lck(ctrlMtx);
            tempStop();
            unregisterInput(_a);
            _a = a;
            registerInput(_a);
            tempStart();
        }

        virtual void setInputB(stream<B>* b) {
            assert(_block_init);
            std::lock_guard<std::recursive_mutex> lck(ctrlMtx);
            tempStop();
            unregisterInput(_b);
            _b = b;
            registerInput(_b);
            tempStart();
        }

        virtual int run() = 0;

        stream<O> out;

    protected:
        stream<A>* _a;
        stream<B>* _b;
    };

} // namespace dsp
