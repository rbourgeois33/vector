//My vector

template<class T, class Allocator = std::allocator<T> /*#?#*/> 
class vector{

    public:

    using value_type = T;
    using size_type = size_t;
    using reference = value_type&;
    using const_reference = const value_type&;

    //======================== Ctors // Dtors
    //default ctor
    vector() : data_(nullptr), size_(0), capacity_(0) {}

    //count ctor
    vector(size_type count) : data_(nullptr), size_(0), capacity_(0){ //not set to count here. State has to be consistent at all time
        
        T* ptr = allocate_raw(count);
        if (!ptr){
            return;
        } else {
            data_=ptr;
        }

        capacity_=count; //eg ok it's allocated now, count is updated

        try {
            for (;size_<count; size_++){ //size_+= 1 for 
                new (data_+size_) T(); //Default initialized
            }
        } catch(...){
            partial_destruction();
        }

    }

    //Fill ctor
    explicit vector( size_type count, const T& value) : data_(nullptr), size_(0), capacity_(0){

        T* ptr = allocate_raw(count);
        if (!ptr){
            return;
        } else {
            data_=ptr;
        }

        capacity_=count;

        try {
            for (;size_<count; size_++){
            //data_[i] = value;  wrong ! means: there is already a valid T living at data_ + i; overwrite its contents. It calls T::operator=,
                new (data_+size_) T(value); //This construct the object T at the desired location but it coult fail T(value) might throw.
            }
        } catch(...){
            partial_destruction();
        }
    }

    //Copy ctor 
    explicit vector( const vector& other ): data_(nullptr), size_(0), capacity_(0) { //explicit: no conversion in type deduction

        T* ptr = allocate_raw(other.size_);
        if (!ptr){
            return;
        } else {
            data_=ptr;
        }

        capacity_=other.capacity_;

        try {
            for (;size_<other.size_; size_++){
                new (data_+size_) T(other[size_]); 
        }
        } catch(...){
            partial_destruction();
        }
    }
    
    //noexcept: cannot throw + something for push back TODO
    //takes a r-value reference AKA an object that is ABOUT to be destroyed
    vector( vector&& other ) noexcept : data_(other.data_), size_(other.size_), capacity_(other.capacity_){

        //when other's dtor is called, nothing is destroyed. We own it now.
        //it's allowed to manipuate other's private attribute since it's the same classe
        other.data_=nullptr;
        other.size_=0;
        other.capacity_=0;
    }
  

    //dtor
    ~vector(){
        for (size_type i=0; i<size_; i++){
            data_[i].~T(); //free objects to avoid leaks (ex: std::strings !!)
        }
        if(data_) free(data_);
    }

    const size_type size() const { //const -> does not change state
        return size_;
    }

     const size_type capacity() const { //const -> does not change state
        return capacity_;
    }

    //======================== accessors[]

    T& operator[](const size_type i){
        return data_[i];
    }

    const T&  operator[](const size_type i) const {
        return data_[i];
    }

    // ====================== = operator 
    // vector already has a pricate state
    // v=v is legal
    // return a reference
    // reuse your capacity
    // dont corrupt target when if throws

    // //deep copy from a reference
    // vector<T>& operator=(const vector<T>& other){

    //     if (capacity_<other.size_){
    //         allocate_raw(other.size_);
    //     }

    //     capacity_=other.size_;

    //     try {
    //         for (;size_<other.size_; size_++){
    //             new (data_+size_) T(other[size_]); 
    //     }
    //     } catch(...){
    //         partial_destruction();
    //     }

    //     return *this; //it means dereference the pointer — access the value stored at the memory address ptr points to.


    // }
    private:
        T* data_{nullptr};
        size_type capacity_{0u};
        size_type size_{0u};

        T* allocate_raw(size_type count){
            if (count==0) return nullptr;
            
            T* new_ptr = static_cast<T*>(malloc(count*sizeof(T))); //malloc returns void*, need to cast
            if (!new_ptr) throw std::bad_alloc(); //not in a catch block
            return new_ptr;
        }

        void partial_destruction(){
            for (size_type j = 0; j < size_; ++j) data_[j].~T(); //Free all succesfully already initiated elements
            free(data_);
            throw; // program stop here, dtor never called. Okay because in a catch bloc
        }

};