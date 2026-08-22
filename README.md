My implementation of `std::vector` for learning sake. Coached by AI but no AI  in `vector.h` (except small identified portions) , while `tests.cpp` is fully AI-generated.

## Lessons i learned / solidified along the way

- [Pointer operators](https://godbolt.org/z/hjbs5TjcT)
- [useless consts](https://godbolt.org/z/3E38G5Tsq)
- [explicit keyword](https://godbolt.org/z/fnPKW1j9M)
- [exceptions, throws, catch](https://godbolt.org/z/89Kd7v8hj)
- [move semantics](https://godbolt.org/z/WzjqYP5e8)
- [noexcept](https://godbolt.org/z/ovr1cb375)
- [std::forward](https://godbolt.org/z/Po81o6rWz)
  
## Vector's state rule to keep in mind

1. size_<=capacity_
2. v[i] for i in 0, size_-1 must be initialized (not malloced bytes but real T's constructed)
3. v[i] for i=size_ -> capacity_-1 must be not initialized, so freed or never initialized
4. data_==nullptr <--> capacity_=0
5. data_ is a pointer to a malloced piece of capacity_*sizeof(T) bytes

## Aliasing in push_back, emplace_back

Both push back and emplace back can have an aliasing problem if the argument is owned by the vector an a reallocation is needed.
