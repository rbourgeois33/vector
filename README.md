# vector

My non-exhaustive implementation of `std::vector` in `vector.h` for learning sake. Some `std::algorithms` threw at it in `algorithms.cpp`.

## Generative AI usage acknowledgement 

Almost no AI in `vector.h` (except small identified portions), while `tests.cpp` is fully AI-generated for the exercise's sake.


## C++ Lessons I learned / solidified along the way

### Vector's state rule (class invariants !) to keep in mind

1. `size_`<=`capacity_`.
2. `v[i]` for `i` in `[0, size_-1`] must be initialized (not just malloc-ed bytes but real T's constructed).
3. `v[i]` for `i` in `[size_,capacity_-1]` must be not initialized, so freed or never initialized.
4. `data_==nullptr` <--> `capacity_=0`
5. if `capacity_>0`,  `data_` is a pointer to a malloced piece of `capacity_*sizeof(T)` bytes.


### Aliasing in push_back, emplace_back
Both push back and emplace back can have an aliasing problem if the argument passed is a reference to an element of the vector an a reallocation is needed.

### general useful concepts

- [Pointer operators](https://godbolt.org/z/hjbs5TjcT)
- [useless consts](https://godbolt.org/z/3E38G5Tsq)
- [explicit keyword](https://godbolt.org/z/fnPKW1j9M)
- [exceptions, throws, catch](https://godbolt.org/z/89Kd7v8hj)
- [move semantics](https://godbolt.org/z/WzjqYP5e8)
- [noexcept](https://godbolt.org/z/ovr1cb375)
- [std::forward](https://godbolt.org/z/Po81o6rWz)
- [std::sort](https://godbolt.org/z/444YTa1rM)
  