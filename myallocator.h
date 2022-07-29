#pragma once
#include<mutex>
#include<iostream>

// 封装了malloc和free操作，可以设置OOM释放内存的回调函数
template <int __inst>
class __malloc_alloc_template {
private:
	static void* _S_oom_malloc(size_t);
	static void* _S_oom_realloc(void*, size_t);
	static void (*__malloc_alloc_oom_handler)();

public:
	static void* allocate(size_t __n)
	{
		void* __result = malloc(__n);
		if (0 == __result) __result = _S_oom_malloc(__n);
		return __result;
	}

	static void deallocate(void* __p, size_t /* __n */)
	{
		free(__p);
	}

	static void* reallocate(void* __p, size_t /* old_sz */, size_t __new_sz)
	{
		void* __result = realloc(__p, __new_sz);
		if (0 == __result) __result = _S_oom_realloc(__p, __new_sz);
		return __result;
	}

	static void (*__set_malloc_handler(void (*__f)()))()
	{
		void (*__old)() = __malloc_alloc_oom_handler;
		__malloc_alloc_oom_handler = __f;
		return(__old);
	}
};

template <int __inst>
void (*__malloc_alloc_template<__inst>::__malloc_alloc_oom_handler)() = nullptr;

template <int __inst>
void* __malloc_alloc_template<__inst>::_S_oom_malloc(size_t __n)
{
	void (*__my_malloc_handler)();
	void* __result;

	for (;;) {
		__my_malloc_handler = __malloc_alloc_oom_handler;
		if (0 == __my_malloc_handler) { throw std::bad_alloc(); }
		(*__my_malloc_handler)();
		__result = malloc(__n);
		if (__result) return(__result);
	}
}

template <int __inst>
void* __malloc_alloc_template<__inst>::_S_oom_realloc(void* __p, size_t __n)
{
	void (*__my_malloc_handler)();
	void* __result;

	for (;;) {
		__my_malloc_handler = __malloc_alloc_oom_handler;
		if (0 == __my_malloc_handler) { throw std::bad_alloc(); }
		(*__my_malloc_handler)();
		__result = realloc(__p, __n);
		if (__result) return(__result);
	}
}
typedef __malloc_alloc_template<0> malloc_alloc;

/*
多线程-线程安全问题
nginx内存池中，一个线程中可以创建独立的nginx内存池来使用，不需要考虑内存池的线程安全问题
移植SGI STL二级空间配置器内存池源码	模板实现
空间配置器=》容器使用的=》容器产生的对象是很有可能在多个线程中去访问操作的
*/

template<typename T>
class myallocator
{
public:
	using value_type = T;

	//	构造函数的模板
	constexpr myallocator() noexcept {}
	constexpr myallocator(const myallocator&) noexcept = default;
	template <class _Other>
	constexpr myallocator(const myallocator<_Other>&) noexcept {}

	//	开辟内存
	T* allocate(size_t __n) {
		__n = __n * sizeof(T);
		void* __ret = 0;

		if (__n > (size_t)_MAX_BYTES) {
			//开辟内存的容量大于阈值128B
			__ret = malloc_alloc::allocate(__n);
		}
		else {
			//开辟内存的容量大于等于阈值128B
			//找到当前申请内存大小的内存块放置的位置（free_list中的位置）
			_Obj* volatile* __my_free_list
				= _S_free_list + _S_freelist_index(__n);
			//skill：使用volatile确保每次读取__my_free_list都是从它的原地址中读取，而不是编译器优化后的位置。
			std::lock_guard<std::mutex> guard(mtx);	//互斥锁封锁作用域

			_Obj* __result = *__my_free_list;
			if (__result == 0)
				//当前位置没有挂载过内存块
				__ret = _S_refill(_S_round_up(__n));
			else {
				*__my_free_list = __result->_M_free_list_link;
				__ret = __result;
			}
		}

		return (T*)__ret;
	}

	//	释放内存
	void deallocate(T* __p, size_t __n) {
		if (__n > (size_t)_MAX_BYTES) {
			malloc_alloc::deallocate(__p, __n);
		}
		else {
			//找到当前待释放内存的内存块放置位置（free_list中的位置）
			_Obj* volatile* __my_free_list
				= _S_free_list + _S_freelist_index(__n);
			//skill：使用volatile确保每次读取__my_free_list都是从它的原地址中读取，而不是编译器优化后的位置。
			_Obj* __q = (_Obj*)__p;
			
			std::lock_guard<std::mutex> guard(mtx);	

			__q->_M_free_list_link = *__my_free_list;
			*__my_free_list = __q;
			// lock is released here
		}
	}

	//内容扩充&缩容
	void* reallocate(void* __p, size_t __old_sz, size_t __new_sz) {
		void* __result;
		size_t __copy_sz;
		//新旧内存容量都大于128B
		if (__old_sz > (size_t)_MAX_BYTES && __new_sz > (size_t)_MAX_BYTES) {
			return(realloc(__p, __new_sz));
		}
		//容量没变化
		if (_S_round_up(__old_sz) == _S_round_up(__new_sz)) return(__p);
		//容量需要操作
		__result = allocate(__new_sz);//分配新内存
		__copy_sz = __new_sz > __old_sz ? __old_sz : __new_sz;//__copy_sz取新旧内存中较小者
		//由_p指向地址为起始地址的连续__copy_sz个字节的数据复制到以__result指向地址为起始地址的空间内
		memcpy(__result, __p, __copy_sz); 
		
		deallocate(__p, __old_sz);//释放旧内存
		return(__result);
	}

	//对象构造
	void construct(T* __p, const T& val) {
		new (__p) T(val);
	}

	//对象析构
	void destory(T* __p) {
		__p->~T();
	}
private:
	/*内存池的粒度信息*/
	enum { _ALIGN = 8 };	//	自由链表是从8字节开始，以8字节为对齐方式，一直扩充到128
	enum { _MAX_BYTES = 128 };	//	内存池最大的chunk块
	enum { _NFREELISTS = 16 }; // 自由链表的个数 = _MAX_BYTES/_ALIGN 

	/*每一个chunk块的头信息*/
	union _Obj {
		union _Obj* _M_free_list_link;	//	存储下一个chunk块的地址
		char _M_client_data[1]; /* The client sees this. */
	};

	// Chunk allocation state.已分配内存chunk块的使用情况
	static char* _S_start_free;
	static char* _S_end_free;
	static size_t _S_heap_size; //内存池大小

	//	_S_free_list表示存储自由链表数组的起始地址
	static _Obj* volatile _S_free_list[_NFREELISTS];

	//	内存池基于freelist实现，需要考虑线程安全，加互斥锁
	static std::mutex mtx;

	/*将 __bytes上调至最邻近的 8 的倍数*/
	static size_t _S_round_up(size_t __bytes) {
		return (((__bytes)+(size_t)_ALIGN - 1) & ~((size_t)_ALIGN - 1));
	}
	/*返回 __bytes 大小的小额区块位于 free-list 中的编号*/
	static size_t _S_freelist_index(size_t __bytes) {
		return (((__bytes)+(size_t)_ALIGN - 1) / (size_t)_ALIGN - 1);
	}

	//将分配好的chunk块进行连接
	static void* _S_refill(size_t __n)
	{
		int __nobjs = 20;
		char* __chunk = _S_chunk_alloc(__n, __nobjs);//分配chunk块
		_Obj* volatile* __my_free_list;
		_Obj* __result;
		_Obj* __current_obj; //free_list上的指针，指向内存块
		_Obj* __next_obj;
		int __i;

		if (1 == __nobjs) return(__chunk);
		__my_free_list = _S_free_list + _S_freelist_index(__n);//确认节点位置

		/* Build free list in chunk */
		__result = (_Obj*)__chunk;
		*__my_free_list = __next_obj = (_Obj*)(__chunk + __n);
		for (__i = 1; ; __i++) {
			__current_obj = __next_obj;
			__next_obj = (_Obj*)((char*)__next_obj + __n);
			if (__nobjs - 1 == __i) {
				__current_obj->_M_free_list_link = 0;//__current_obj指向最后一块内存块
				break;
			}
			else {
				__current_obj->_M_free_list_link = __next_obj;
			}
		}
		return(__result);
	}

	//主要负责分配自由链表，chunk块
	static char* _S_chunk_alloc(size_t __size, int& __nobjs)
	{
		char* __result;
		size_t __total_bytes = __size * __nobjs;
		size_t __bytes_left = _S_end_free - _S_start_free;//查询当前内存池余额

		if (__bytes_left >= __total_bytes) {
			//有大块内存，进行整块分配
			__result = _S_start_free;
			_S_start_free += __total_bytes;
			return(__result);
		}
		else if (__bytes_left >= __size) {
			//有小块（大于内存块需求）内存进行连续分配
			__nobjs = (int)(__bytes_left / __size);
			__total_bytes = __size * __nobjs;
			__result = _S_start_free;
			_S_start_free += __total_bytes;
			return(__result);
		}
		else {
			size_t __bytes_to_get =
				2 * __total_bytes + _S_round_up(_S_heap_size >> 4);
			// Try to make use of the left-over piece.对剩余小块内存重新利用
			if (__bytes_left > 0) {
				_Obj* volatile* __my_free_list =
					_S_free_list + _S_freelist_index(__bytes_left);

				//当前自由链表节点指向真正的内存位置
				((_Obj*)_S_start_free)->_M_free_list_link = *__my_free_list;
				*__my_free_list = (_Obj*)_S_start_free;
			}
			_S_start_free = (char*)malloc(__bytes_to_get); //使用malloc一级空间配置器再次分配内存
			//malloc内存申请失败
			if (nullptr == _S_start_free) {
				size_t __i;
				_Obj* volatile* __my_free_list;
				_Obj* __p;
				// Try to make do with what we have.  That can't
				// hurt.  We do not try smaller requests, since that tends
				// to result in disaster on multi-process machines.
				for (__i = __size;
					__i <= (size_t)_MAX_BYTES;
					__i += (size_t)_ALIGN) {
					__my_free_list = _S_free_list + _S_freelist_index(__i);
					__p = *__my_free_list;
					if (0 != __p) {
						*__my_free_list = __p->_M_free_list_link;
						_S_start_free = (char*)__p;
						_S_end_free = _S_start_free + __i;
						return(_S_chunk_alloc(__size, __nobjs));
						// Any leftover piece will eventually make it to the
						// right free list.
					}
				}
				_S_end_free = 0;	// In case of exception.
				_S_start_free = (char*)malloc_alloc::allocate(__bytes_to_get);
				// This should either throw an
				// exception or remedy the situation.  Thus we assume it
				// succeeded.
			}
			_S_heap_size += __bytes_to_get;
			_S_end_free = _S_start_free + __bytes_to_get;
			return(_S_chunk_alloc(__size, __nobjs));
		}
	}
};

template <typename T>
char* myallocator<T>::_S_start_free = nullptr;

template <typename T>
char* myallocator<T>::_S_end_free = nullptr;

template <typename T>
size_t myallocator<T>::_S_heap_size = 0;

template <typename T>
typename  myallocator<T>::_Obj* volatile myallocator<T>::_S_free_list[_NFREELISTS]{
	nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr
};

template <typename T>
std::mutex mtx;
