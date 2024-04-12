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


using size_type = std::make_signed_t<std::size_t>;

template <size_type N>
struct index_type_traits
{
	static_assert(N >= 0 && N <= std::numeric_limits<std::size_t>::max());
	using type = std::size_t;
};

template <size_type N>
	requires (N >= 0 && N <= std::numeric_limits<unsigned int>::max())
struct index_type_traits<N> { using type = unsigned int; };

template <size_type N>
using index_type = struct index_type_traits<N>::type;


export constexpr struct {} no_sentinel_value = {};

template <typename T, auto sentinel_value = no_sentinel_value>
struct ringbuffer_slot
{
	static_assert(std::convertible_to<T, decltype(sentinel_value)>);

	alignas(std::hardware_destructive_interference_size) std::atomic<T> value = sentinel_value;
};

template <typename T>
class ringbuffer_slot<T, no_sentinel_value>
{
	alignas(std::hardware_destructive_interference_size) std::atomic_flag has_value = false;
	union { T value; };

public:
	ringbuffer_slot() = default;

	ringbuffer_slot(auto&& value)
		: value(std::forward<decltype(value)>(value)),
		  has_value(true)
	{
	}

	~ringbuffer_slot()
	{
		if (has_value.test(std::memory_order::relaxed))  // TODO: non-atomic load
			std::destroy_at(&value);
	}

	void put(auto&& value, std::memory_order order = std::memory_order::release) requires requires { std::construct_at(&value, std::forward<decltype(value)>(value)); }
	{
		std::construct_at(&value, std::forward<decltype(value)>(value));

		[[maybe_unused]] bool had_value = has_value.test_and_set(order);
		assert(!had_value);
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
struct ringbuffer
{
	using index_type = ::index_type<N>;

	static consteval auto size() { return static_cast<size_type>(std::bit_ceil(static_cast<index_type>(N))); }

	ringbuffer_slot<T, sentinel_value> slots[size()];

	decltype(auto) operator [](this auto&& self, index_type i)
	{
		return slots[i % static_cast<index_type>(size())];
	}
};


// externally bounded queue:
// external circumstances guarantee that there are never more items in flight than fit in the queue.
// therefore, tail can never overtake head.
// 	=> push always has space
// 	=> no need to track size, or check upon push
// 	=> no need to acquire slot upon store, slot is always free

// blocking vs non-blocking queue:
// don't just offer both push/pop and try_push/try_pop.
// knowing the other operation will never need to wait allows for avoiding notify calls.

// bulk operations:
// store locks separately to have contiguous region or elements
// how do we handle wrap around?

export template <typename T, size_type N, auto sentinel_value = no_sentinel_value>
class mpsc_externally_bounded_queue
{
	ringbuffer<T, N + 1> ringbuffer;  // allocate one more slot to differentiate empty from full

	alignas(std::hardware_destructive_interference_size) std::atomic<unsigned int> tail = 0;
	alignas(std::hardware_destructive_interference_size) unsigned int head = 0;

public:
	mpsc_no_overflow_ringbuffer_queue(auto&&... elements) requires (sizeof...(elements) <= N)
		: data{std::forward<decltype(elements)>(elements)...},
		  tail(sizeof...(elements))
	{
	}

	void push(auto&& value) requires requires()
	{
		ringbuffer[tail.fetch_add(1, std::memory_order::release)].put(std::forward<decltype(value)>(value));
		tail.notify_one();
	}

	T pop()
	{
		while (true)
		{
			auto last_tail = tail.load(std::memory_order::acquire);

			if (&ringbuffer[head] != &ringbuffer[last_tail])
				return std::move(ringbuffer[head++]).load();

			tail.wait(last_tail, std::memory_order::relaxed);
		}
	}
};


export template <typename T, int N>
class io_buffer_queue
{
	// multi producer: processing thread pool
	// single consumer: io thread
	// we know that there are never more items in flight than fit in the queue
	// therefore, tail can never overtake head

public:
	io_buffer_queue(auto&&... elements) requires (sizeof...(elements) == N)
		: data{std::forward<decltype(elements)>(elements)...},
		  tail(sizeof...(elements))
	{
	}

	class item final
	{
		friend class io_buffer_queue;

		T value;
		io_buffer_queue* q;

		explicit item(auto&& value, io_buffer_queue* q = nullptr)
			: value(std::forward<decltype(value)>(value)), q(q)
		{
		}

	public:
		item(item&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
			: value(std::move(other.value)), q(std::exchange(other.q, nullptr))
		{
		}

		item& operator =(item&& other) noexcept(std::is_nothrow_move_constructible_v<item>)
		{
			~item();
			return *new (this) item(std::move(other));
		}

		~item()
		{
			if (q)
				q->push(std::move(value));
		}

		operator const T&() const { return value; }

		T release() &&
		{
			auto oq = std::exchange(q, nullptr);

			try
			{
				return std::move(value);
			}
			catch (...)
			{
				q = oq; throw;
			}
		}
	};

	item pop()
	{
		while (true)
		{
			auto last_tail = tail.load(std::memory_order::acquire);

			if (wrap(head) != wrap(last_tail))
				return item(std::move(data[wrap(head++)]), this);

			tail.wait(last_tail, std::memory_order::relaxed);
		}
	}

	item reacquire(T&& value)
	{
		return item(std::move(value), this);
	}
};
