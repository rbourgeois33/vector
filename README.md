My implementation of `std::vector` for learning sake. No AI (except small identified portions) in `vector.h`, while `tests.cpp` is fully AI-generated.

## Lessons

- [Pointer operators](https://godbolt.org/z/hjbs5TjcT)
- [useless consts](https://godbolt.org/z/3E38G5Tsq)
- [explicit keyword](https://godbolt.org/z/fnPKW1j9M)
- [exceptions, throws, catch](https://godbolt.org/z/89Kd7v8hj)
- [move semantics](https://godbolt.org/z/WzjqYP5e8)
- [noexcept](https://godbolt.org/z/ovr1cb375)
  
## Vector's state rule to keep in mind

1. size_<=capacity_
2. v[i] for i in 0, size_-1 must be initialized (not malloced bytes but real T's constructed)
3. v[i] for i=size_ -> capacity_-1 must be not initialized, so freed or never initialized
4. data_==nullptr <--> capacity_=0
5. data_ is a pointer to a malloced piece of capacity_*sizeof(T) bytes
