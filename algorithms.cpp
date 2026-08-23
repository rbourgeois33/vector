#include "vector.h"
#include <algorithm>
#include <numeric>
#include <execution>


//wrong to check what happens, not associative, will vary
struct my_custom_op{
    int operator()(int a, int b){
        return -10;
    }
};

struct reverse{
    bool operator()(int a, int b){
        return a>b;
    }
};

struct sort_according_to_another_vector{

    vector<int> vector;

    bool operator()(int ia, int ib){
        return vector[ia] < vector[ib];
    }
};
int main(){

    vector<int> v;

    for (int i=0; i<10; i++){
        v.push_back(10-i);
    }

    v.print();

    int sum = std::reduce(v.begin(), v.end(), 0, std::plus<>());
    std::cout<<"sum= "<< sum<<std::endl;

    int seeded_sum = std::reduce(v.begin(), v.end(), 10, std::plus<>());
    std::cout<<"seeded_sum= "<< seeded_sum<<std::endl;


    int custom_sum = std::reduce(v.begin(), v.end(), 10, my_custom_op()); //op is not associative: UB, result vary with impl & policy
    std::cout<<"custom_sum= "<< custom_sum<<std::endl;

    
    //seq: seq
    //unseq: simd
    //par: openmp style share of the work
    //par_unseq: both
    int fast_sum = std::reduce(std::execution::par, v.begin(), v.end(), 0, std::plus<>());
    std::cout<<"fast_sum= "<< fast_sum<<std::endl;

    std::sort(v.begin(), v.end());
    v.print();

    std::sort(std::execution::par, v.begin(), v.end(), reverse());
    v.print();

    vector<int> another_vector;

    for (int i=0; i<10; i++){
        auto x = (i%2) ? i :-i;
        another_vector.push_back(x);
    }


    std::cout<<"another vector\n";

    another_vector.print();
    vector<int> index(10), out(10);

    std::iota(index.begin(), index.end(), 0);
    index.print();
    std::sort(std::execution::par, index.begin(), index.end(), sort_according_to_another_vector{another_vector}); //no () there Oo
    std::cout<<"sorted index:\n";
    index.print();

    //put back into out
    std::transform(index.begin(), index.end(), /*not v here !*/ out.begin(), [&](int i) { return v[i]; });

    out.print();


    //in place transformation
    //no return
    //[](int &a){ return (a<0) ? -a:a;} does nothing (a never touched)
    //[](int a){ {a=(a<0) ? -a:a;} does nothing (only local copy is modified)
    std::for_each(another_vector.begin(), another_vector.end(), [](int &a){a=(a<0) ? -a:a;});
    another_vector.print();


    vector<int> result(10);
    std::sort(v.begin(), v.end());
    v.print();

    //this time a return is needed bc we write somewhere else
    std::transform(v.begin(), v.end(), result.begin(), [](int&a){return a+1;});
    result.print();



    for (int i=0; i<10; i++){
        v[i] = 0;
    }

    
   v.print();

    int result_ = std::transform_reduce(v.begin(), v.end(), 0,
                            [](int a, int b){return a+b;}, //reduce
                            [](int&a){return a+1;} // transform
                         );
    
   std::cout<<result_<<std::endl;
   //important: v is not transformed! (no W)
   v.print();

    for (int i=0; i<10; i++){
        v[i] = 1;
    }
    v.print();

    // transform version too
   std::inclusive_scan(v.begin(), v.end(), result.begin(), std::plus<>());
   result.print();
}