//My vector, no AI

// Yet to be understood
// you only need cleanup where a failure would leave the invariant broken OK
//Explicit keyword, and not on copy ctor
//noexcept keyword on move ctor
//understand         if (this == &other) return *this OK

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
            destroy_all_and_throw();
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
            destroy_all_and_throw();
        }
    }

    //Copy ctor 
    vector( const vector& other ): data_(nullptr), size_(0), capacity_(0) { //explicit: no conversion in type deduction

        T* ptr = allocate_raw(other.size_);
        if (!ptr){
            return;
        } else {
            data_=ptr;
        }

        capacity_=other.size_;

        try {
            for (;size_<other.size_; size_++){
                new (data_+size_) T(other[size_]); 
        }
        } catch(...){
            destroy_all_and_throw();
        }
    }
    
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
        destroy_all();
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
    vector<T>& operator=(const vector<T>& other){

        if (this == &other) return *this;

        if (capacity_<other.size_){

            //Build new THEN destroy THEN metadata
            //build new
            T* new_ptr = allocate_raw(other.size_);
            
            size_type built; 
            //Could fail : = operator
            try {
                for (built=0;built<other.size_; built++){ 
                    new (new_ptr+built) T(other[built]); //Default initialized
                }
            } catch(...){
                for (size_type j = 0; j < built; ++j) new_ptr[j].~T();
                free(new_ptr);
                throw;                        // *this is completely untouched
            }

            //2 
            destroy_all(); 
            //3
            data_=new_ptr; 
            capacity_ = other.size_;
            size_=built;

            return *this;
        }

        //here our elements are initialized until size_ 
        if (size_<other.size_){ //Never happens if reallocated

            //= for first elements, no state lie

            try{
            for (size_type i =0;i<size_; i++){ // no catch for =
                data_[i] = other[i]; //= operator on already initialized element
            }
            } catch (...){
                throw;
            }

            //new ctr for after elements
             try{
            for (;size_<other.size_; size_++){
                new (data_+size_) T(other[size_]); 
            }
            } catch(...){
                throw;
            }
        }
        else{
            //size_ > other.size_

            for(size_type i=other.size_; i<size_; i++){ // destroy tail elements
                data_[i].~T(); // will not throw
            }

            try{
                for (size_type i=0;i<other.size_; i++){ // no catch for =
                    data_[i] = other[i]; //= operator on already initialized element
                }
            } catch(...){
                throw; //No destroy ! because state is still valid
            }

       

            size_=other.size_;
          
        }
        return *this; 
    }

    //Get rid of our stuff
    //Take the source buffer, leave it destructible
    vector<T>& operator=(vector<T>&& other) noexcept{
        if (this == &other) return *this;

        destroy_all();

        data_=other.data_;
        capacity_=other.capacity_;
        size_=other.size_;

        //when other's dtor is called, nothing is destroyed. We own it now.
        // When other is destroyed, it wont free its original ptr
        other.data_=nullptr;
        other.size_=0;
        other.capacity_=0;

        return *this;
    }

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

        //Cleanup helper 
        void destroy_all_and_throw(){
            destroy_all();
            throw; // program stop here, dtor never called. Okay because in a catch bloc
        }

        //deconstrut until size_
        void destroy_all() {
            for (size_type j = 0; j < size_; ++j) data_[j].~T();
            if (data_) free(data_);
            data_ = nullptr;
            size_ = 0;
            capacity_ = 0;
        }



};

/*
#include<iostream>
int main(){

    int a =5;
    int& b=a;

    int*a_ptr = &a; //& on a value -> pointer

    std::cout<<a<<std::endl;
    std::cout<<b<<std::endl;
    std::cout<<a_ptr<<std::endl;
    std::cout<<&b<<std::endl; //& on a reference -> pointer
    std::cout<<&a<<std::endl; //& on a reference -> pointer
    std::cout<<*a_ptr<<std::endl; // *on a pointer -> object

    //donc &other when other is a ref: pointer, this=pointer of the current object, *this -> object then passed by reference. same code for object return

    return 0;
}
*/