// Yet to be understood
// you only need cleanup where a failure would leave the invariant broken; Understand exactly where catch + throw + destroy is needed or not
// Explicit keyword, and not on copy ctor
// noexcept keyword on move ctor
// does throw early returns ?
// what is allocator
// understand & fix max_size()  (ptr diff)

// done
// understand if (this == &other) return *this OK


template<class T> 
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
    explicit vector( size_type count, const_reference value) : data_(nullptr), size_(0), capacity_(0){

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
            T* new_ptr = allocate_raw(other.size_);
            
            size_type built=0; 
            //Could fail : = operator
            try {
                for (;built<other.size_; built++){ 
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

            for(size_type i=other.size_; i<size_; i++){ // destroy tail elements
                data_[i].~T(); // will not throw
            }

            //No destroy ! because state is still valid
            for (size_type i=0;i<other.size_; i++){ // no catch for =
                data_[i] = other[i]; //= operator on already initialized element
            }

            size_=other.size_;
          
        }
        return *this; 
    }

    //= with a vector that is about to disappear same as constrcutor but we get rid of our stuff first.
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

    // =============================== capacity 

    bool empty() const{
        return size_==0;
    }

    const size_type size() const { //const -> does not change state
        return size_;
    }

    const size_type capacity() const { //const -> does not change state
        return capacity_;
    }

    size_type max_size() const {
        return std::numeric_limits<size_type>::max() / sizeof(T);
    }

    //only useful if (new_cap>capacity_)
    void reserve(size_type new_cap){
        if (new_cap>capacity_) reallocate(new_cap);
    }

    //causes reallocation !! only useful if (capacity_ > size_) 
    void shrink_to_fit(){
        if (capacity_ > size_) reallocate(size_);
    }

    private:
        T* data_{nullptr};
        size_type capacity_{0u};
        size_type size_{0u};

        T* allocate_raw(size_type count){
            if (count==0) return nullptr;
            if (count >= max_size()) throw std::bad_alloc();
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

        void reallocate(size_type new_cap){
            //Build new THEN destroy THEN metadata

            assert(new_cap>=size_);
            T* new_ptr = allocate_raw(new_cap);
                
            size_type built=0; 
            //Could fail : = operator
            //choice: new items that are > size but < capacity are no even initialized ?
            try {
                for (;built<size_; built++){ 
                // new (new_ptr+built) T(data_[built]); //Default initialized mistake because it rebuilds !! same as interview
                    new (new_ptr + built) T(std::move_if_noexcept(data_[built])); // calls the move constructor, no rebuild (AI this one)
            }
            } catch(...){
                for (size_type j = 0; j < built; ++j) new_ptr[j].~T();
                free(new_ptr);
                throw;                        // *this is completely untouched
            }

            //2 
            size_type old_size = size_;
            destroy_all(); // killing size is too mich here, we save it before hands
            size_ = old_size;

            //3
            data_=new_ptr; 
            capacity_ = new_cap;
        }

};

/*
#include<iostream>
int main(){

    int a =5;
    inreference b=a;

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