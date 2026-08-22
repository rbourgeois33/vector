// (almost) no AI code in here
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <cassert>
#include <utility>
#include <cstdlib>


#ifndef VECTOR_INCLUDED
#define VECTOR_INCLUDED

template<class T> 
class vector{

    public:

    using value_type = T;
    using size_type = size_t;
    using reference = value_type&;
    using const_reference = const value_type&;

    //======================== Ctors // Dtors
    //default ctor
    vector() noexcept : data_(nullptr), size_(0), capacity_(0) /*same order as declaration in private */{}

    //count ctor. Explicit needed because size_type -> std::vector<size_t> can happen
    //explicit: no implicit conversion these could happen
    //void print(const vector<int>& v);
    //print(42);                 // compiles — builds a 42-element vector
    //vector<int> v;
    //v = 5;                     // compiles — temp vector of 5 zeros, then move-assign. 
    //vector<int> w = 10;        // compiles — looks like "w holds the value 10"
    explicit vector(size_type count) : data_(nullptr), size_(0), capacity_(0){ //not set to count here. State has to be consistent at all time
        
        T* ptr = allocate_raw_(count);
        if (!ptr){
            return;
        } else {
            data_=ptr;
        }

        capacity_=count; //rule V

        try {
            for (;size_<count; size_++){ //at all times, II is verified ...
                new (data_+size_) T(); //Default initialized
            }
        } catch(...){
            //If any allocation throws, detroy all element util size_(not state lie, II) free ptr and attributes and propagate the throw
            destroy_all_(); throw;;
        }

    }

    //value ctor
    vector( size_type count, const_reference value) : data_(nullptr), size_(0), capacity_(0){

        T* ptr = allocate_raw_(count);
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
            destroy_all_(); throw;
        }
    }

    //Copy ctor 
    //explicit: controls whether a constructor is usable for implicit conversions and copy-initialization. We want that:
    //vector<int> b = a;              // copy-initialization — dead
    //vector<int> f() { return v; }   // the return is copy-init too — dead
    vector( const vector& other ): data_(nullptr), size_(0), capacity_(0) { 

        T* ptr = allocate_raw_(other.size_);
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
            destroy_all_(); throw;;
        }
    }
    
    //Move constructor
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
        destroy_all_();
    }

    //================ element access

    reference operator[](const size_type i){
        return data_[i];
    }

    const_reference  operator[](const size_type i) const {
        return data_[i];
    }

    reference at(const size_type i){
        if (i >= size_) {
            throw std::out_of_range("Index is out of range");
        }
        return data_[i];
    }

    const_reference at(const size_type i) const {
        if (i >= size_) {
            throw std::out_of_range("Index is out of range");
        }
        return data_[i];
    }

    reference front(){
        if (empty()) {
            throw std::out_of_range("vector is not allocated");
        }
        return data_[0];
    }

    const_reference front() const {
        if (empty()) {
            throw std::out_of_range("vector is not allocated");
        }
        return data_[0];
    }

    reference back(){
        if (empty()) {
            throw std::out_of_range("vector is not allocated");
        }
        return data_[size_-1];
    }

    const_reference back() const {
        if (empty()) {
            throw std::out_of_range("vector is not allocated");
        }
        return data_[size_-1];
    }

    const T* data() const {
        return data_;
    }

    T* data() {
        return data_;
    }



    // ====================== = operator 
    // vector already has a private state
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
            T* new_ptr = allocate_raw_(other.size_);
            
            size_type built=0; 
            //Could fail : = operator
            try {
                for (;built<other.size_; built++){ 
                    new (new_ptr+built) T(other[built]); 
                }
            } catch(...){
                //if any of the ctor above throw, we destroy the thing we are constructing (all the T up to built & ptr), then propagate the throw
                for (size_type j = 0; j < built; ++j) new_ptr[j].~T();
                free(new_ptr);
                throw;  //Throws either jump straight to the potential calling catch block. Rest of the function is not executed
            }
            //2 
            destroy_all_(); //never reached if it threw
            //3
            data_=new_ptr; 
            capacity_ = other.size_;
            size_=built;

        }else // in this branch no catch bc a handler earns its place only when something would leak or lie without it
        {
            //here our elements are initialized until size_ 
            if (size_<other.size_){ //Never happens if reallocated

                //= for first elements, no state lie
                for (size_type i =0;i<size_; i++){ // no catch for =
                    data_[i] = other[i]; //= operator on already initialized element
                }
                //new ctr for after elements
                for (;size_<other.size_; size_++){
                    new (data_+size_) T(other[size_]); 
                }
            }
            else{
                //size_ > other.size_
                for(size_type i=other.size_; i<size_; i++){ // Rule III
                    data_[i].~T(); // will not throw (usually noexcept)
                }

                size_=other.size_; //must be here not below !!! otherwise if it trhows, double free some elements

                //No destroy ! because state is still valid
                for (size_type i=0;i<other.size_; i++){ // no catch for =
                    data_[i] = other[i]; //= operator on already initialized element
                }

                //size_=other.size_;
            
            }
            
        }
        return *this; 
    }

    //Move =
    //= with a vector that is about to disappear same as constrcutor but we get rid of our stuff first.
    vector<T>& operator=(vector<T>&& other) noexcept{
        if (this == &other) return *this;

        destroy_all_();

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

    // =============================== capacity 

    bool empty()  const noexcept {
        return size_==0;
    }

    size_type size() const noexcept { //const -> does not change state, useless for value return 
        return size_;
    }

    size_type capacity()  const noexcept { //const -> does not change state
        return capacity_;
    }

    //AI: a little confused about this one
    constexpr size_type max_size()  const noexcept{
        //max value for signed ptr in bytes so that arithmetic stays within bound, / by sizeof(T)
        return std::numeric_limits<ptrdiff_t>::max() / sizeof(T);
    }

    //only useful if (new_cap>capacity_)
    void reserve(size_type new_cap){
        if (new_cap>capacity_) reallocate_(new_cap);
    }

    //causes reallocation !! only useful if (capacity_ > size_) 
    void shrink_to_fit(){
        if (capacity_ > size_) reallocate_(size_);
    }

    //======================= modifiers
    //destuctors are noexcept
    void clear() noexcept {
        //rule II is enforced
        for (size_type j = 0; j < size_; ++j) data_[j].~T(); 
            size_ = 0;
    }

    //The mighty
    //Appends a new element to the end of the container. The element is constructed through std::allocator_traits::construct, which typically uses placement-new to construct the element in-place 
    //at the location provided by the container. The arguments args... are forwarded to the constructor as std::forward<Args>(args).... 
    //If after the operation the new size() is greater than old capacity() a reallocation takes place,
    // universal ref bc we might call copy or move ctor
    //move_if_noexcept explained below
    //done better than push back bc constructor is called just once
    template< class... Args >
    reference emplace_back( Args&&... args ){

        if (size_==capacity_){

            size_t new_cap = new_cap_policy_();
            T* new_ptr = allocate_raw_(new_cap);


            //to avoid aliasing issue if args is v[0], it will be moved later !!
            try{
                 new (new_ptr + size_) T(std::forward<Args>(args)...);
            }catch(...){
                //new_ptr[size_].~T();  no need, placement new threw
                free(new_ptr);
                throw;
            }

            size_type built=0; 

            //Now the last element is built, we build the other, not a problem if data[built] corresponds to args and is moved now
            try {
                for (;built<size_; built++){ 
                    new (new_ptr + built) T(std::move_if_noexcept(data_[built]));
                }
            } catch(...){ // never happnes is move ctor is noexcept
                for (size_type j = 0; j < built; ++j) new_ptr[j].~T(); 
                new_ptr[size_].~T();      // the element built in the first try
                free(new_ptr);
                throw;
            }

            //destroy current state
            size_type old_size = size_;
            destroy_all_(); // killing size is too mich here, we save it before hands
            
            //update metatdata
            //enforce II, III, V at the same time
            size_=old_size+1;
            data_=new_ptr; 
            capacity_ = new_cap;

            return data_[size_-1];
        }

        new (data_+size_) T(std::forward<Args>(args)...);
        size_+=1;
        return data_[size_-1];
    }
    //it's just a special case of emplace_back where the constructor is copy, and argument value
    void push_back( const T& value ){emplace_back(value);}
    //Moved version
    //call v.push_back(5). 5 is moved into value, has to be re-moved into emplace back
    void push_back(T&& value ){emplace_back(std::move(value));}


    // ============ private

    private:
        T* data_{nullptr};
        size_type size_{0u};
        size_type capacity_{0u};

        size_type new_cap_policy_(){
            return capacity_==0 ? 1:capacity_*2;
        }

        //const here ! it does not touch internal state
        T* allocate_raw_(size_type count) const{
            if (count==0) return nullptr;
            if (count > max_size()) throw std::length_error("too large"); //not >
            T* new_ptr = static_cast<T*>(malloc(count*sizeof(T))); //malloc returns void*, need to cast
            if (!new_ptr) throw std::bad_alloc(); //not in a catch block
            return new_ptr;
        }

        //deconstrut until size_
        void destroy_all_() noexcept {
            clear();
            if (data_) free(data_); // then rule IV
            data_ = nullptr;
            capacity_ = 0;
        }

        //internally only used in push_back, emplace_back
        //only occurence of move_if_noexcept
        // Why not move only ? if it can except, we free previously moved element ## and they will be refreed
        // why not always copy ? If we can save some time by moveing it's better
        void reallocate_(size_type new_cap){
            //Build new THEN destroy THEN metadata

            assert(new_cap>=size_);
            T* new_ptr = allocate_raw_(new_cap);
                
            size_type built=0; 
            try {
                for (;built<size_; built++){ 
                // new (new_ptr+built) T(data_[built]); //Default initialized mistake because it rebuilds !! same as interview
                    new (new_ptr + built) T(std::move_if_noexcept(data_[built])); //AI
                    // if T's move ctor is noexcept it moves, otherwise it copies
            }
            } catch(...){ // never happnes is move ctor is noexcept
                for (size_type j = 0; j < built; ++j) new_ptr[j].~T(); 
                free(new_ptr);
                throw;
            }

            //2 
            size_type old_size = size_;
            destroy_all_(); // killing size is too mich here, we save it before hands
            
            //3
            //enforce II, III, V at the same time
            size_ = old_size;
            data_=new_ptr; 
            capacity_ = new_cap;
        }

};


#endif //VECTOR_INCLUDED


