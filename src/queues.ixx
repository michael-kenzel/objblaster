module;
#include <cassert>
#include <type_traits>
#include <concepts>
#include <limits>
#include <cstddef>
#include <utility>
#include <new>
#include <memory>
#include <bit>
#include <atomic>

export module queues;


// Design Considerations
//
// externally bounded queue:
// external circumstances guarantee that there are never more items in flight than fit in the queue.
// therefore, tail can never overtake head.
// 	=> push can never fail
// 	=> no need to track size, or check size upon push
// 	=> no need to acquire slot upon store, slot is always free
//
// single producer/single consumer + externally bounded:
// 	=> one end of the queue can be operated non-atomically
//
// blocking vs non-blocking queue:
// don't just offer both push/pop and try_push/try_pop.
// knowing the other operation will never need to wait allows for avoiding notify calls.
//
// bulk operations:
// store locks separately to have contiguous region or elements
// how do we handle wrap around?


using size_type = std::make_signed_t<std::size_t>;

export template <typename T, size_type N>
struct ring_buffer
{
	static consteval auto size()
	{
		static_assert(N > 0);

		static constexpr auto pot_size = std::bit_ceil(static_cast<std::make_unsigned_t<decltype(N)>>(N));

		using size_type =
			std::conditional_t<pot_size <= std::numeric_limits<int>::max(), int,
				std::conditional_t<pot_size <= std::numeric_limits<long>::max(), long,
					long long>>;

		static_assert(pot_size <= std::numeric_limits<size_type>::max());

		return static_cast<size_type>(pot_size);
	}

	using index_type = std::make_unsigned_t<decltype(size())>;
	using element_type = T;

	T slots[size()];

	decltype(auto) operator [](this auto&& self, index_type i)
	{
		return slots[i % static_cast<index_type>(size())];
	}
};

export constexpr struct {} no_sentinel_value = {};

template <typename T, bool blocking_put, bool blocking_take, auto sentinel_value = no_sentinel_value>
struct concurrent_queue_slot
{
	static_assert(std::convertible_to<T, decltype(sentinel_value)>);

	alignas(std::hardware_destructive_interference_size) std::atomic<T> value = sentinel_value;
};

template <typename T, bool blocking_put, bool blocking_take>
class concurrent_queue_slot<T, blocking_put, blocking_take, no_sentinel_value>
{
	alignas(std::hardware_destructive_interference_size) std::atomic_flag has_value = false;
	union { T value; };

public:
	concurrent_queue_slot() = default;

	concurrent_queue_slot(auto&& value)
		: value(std::forward<decltype(value)>(value)),
		  has_value(true)
	{
	}

	~concurrent_queue_slot()
	{
		if (has_value.test(std::memory_order::relaxed))  // TODO: non-atomic load
			std::destroy_at(&value);
	}

	template <std::memory_order order = blocking_put ? std::memory_order::acq_rel : std::memory_order::release>
	void put(auto&& value)
		requires requires { std::construct_at(&value, std::forward<decltype(value)>(value)); }
	{
		if (blocking_put)
		{
		}

		std::construct_at(&value, std::forward<decltype(value)>(value));

		[[maybe_unused]] bool had_value = has_value.test_and_set(order);
		assert(!had_value);

		if constexpr (blocking_take)
			has_value.notify_one();
	}

	T wait_and_take(this auto&& self, std::memory_order order = std::memory_order::acquire)
	{
		while (true)
		{
			if (has_value.test(order))
				return
		}
	}
};

export template <typename T, size_type N, auto sentinel_value = no_sentinel_value>
class mpsc_externally_bounded_queue
{
	using slot_type = concurrent_queue_slot<T, false, true, sentinel_value>;

	ring_buffer<slot_type, N + 1> ring_buffer;  // allocate one more slot to differentiate empty from full

	alignas(std::hardware_destructive_interference_size) std::atomic<unsigned int> tail = 0;
	alignas(std::hardware_destructive_interference_size) unsigned int head = 0;

public:
	mpsc_externally_bounded_queue(auto&&... elements) requires (sizeof...(elements) <= N)
		: data{std::forward<decltype(elements)>(elements)...},
		  tail(sizeof...(elements))
	{
	}

	void push(auto&& value) noexcept
		requires requires { ring_buffer[tail.fetch_add(1, std::memory_order::release)].put(std::forward<decltype(value)>(value)); }
	{
		ring_buffer[tail.fetch_add(1, std::memory_order::release)].put(std::forward<decltype(value)>(value));
		tail.notify_one();
	}

	T pop()
	{
		while (true)
		{
			auto last_tail = tail.load(std::memory_order::acquire);

			if (&ring_buffer[head] != &ring_buffer[last_tail])
				return std::move(ring_buffer[head++]).load();

			tail.wait(last_tail, std::memory_order::relaxed);
		}
	}
};


// export template <typename T, int N>
// class io_buffer_queue
// {
// 	// multi producer: processing thread pool
// 	// single consumer: io thread
// 	// we know that there are never more items in flight than fit in the queue
// 	// therefore, tail can never overtake head

// public:
// 	io_buffer_queue(auto&&... elements) requires (sizeof...(elements) == N)
// 		: data{std::forward<decltype(elements)>(elements)...},
// 		  tail(sizeof...(elements))
// 	{
// 	}

// 	class item final
// 	{
// 		friend class io_buffer_queue;

// 		T value;
// 		io_buffer_queue* q;

// 		explicit item(auto&& value, io_buffer_queue* q = nullptr)
// 			: value(std::forward<decltype(value)>(value)), q(q)
// 		{
// 		}

// 	public:
// 		item(item&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
// 			: value(std::move(other.value)), q(std::exchange(other.q, nullptr))
// 		{
// 		}

// 		item& operator =(item&& other) noexcept(std::is_nothrow_move_constructible_v<item>)
// 		{
// 			~item();
// 			return *new (this) item(std::move(other));
// 		}

// 		~item()
// 		{
// 			if (q)
// 				q->push(std::move(value));
// 		}

// 		operator const T&() const { return value; }

// 		T release() &&
// 		{
// 			auto oq = std::exchange(q, nullptr);

// 			try
// 			{
// 				return std::move(value);
// 			}
// 			catch (...)
// 			{
// 				q = oq; throw;
// 			}
// 		}
// 	};

// 	item pop()
// 	{
// 		while (true)
// 		{
// 			auto last_tail = tail.load(std::memory_order::acquire);

// 			if (wrap(head) != wrap(last_tail))
// 				return item(std::move(data[wrap(head++)]), this);

// 			tail.wait(last_tail, std::memory_order::relaxed);
// 		}
// 	}

// 	item reacquire(T&& value)
// 	{
// 		return item(std::move(value), this);
// 	}
// };
