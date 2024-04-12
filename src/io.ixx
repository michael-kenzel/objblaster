module;
#include <exception>
#include <cstddef>
#include <memory>
#include <cmath>
#include <span>
#include <string_view>
#include <string>
#include <numeric>
#include <atomic>
#include <thread>
#include <filesystem>
#include <iostream>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NTDDI_VERSION NTDDI_WIN10_NI
#include <Windows.h>
#include <winioctl.h>
#include <ioringapi.h>

export module io;

import queues;

using namespace std::literals;


export struct win32error : std::exception
{
	HRESULT error_code;

	inline win32error(HRESULT error_code) : error_code(error_code) {}

	inline const char* what() const noexcept override { return "Win32 error"; }
};

export inline HRESULT throw_error(HRESULT res)
{
	if (!SUCCEEDED(res))
		throw win32error(res);
	return res;
}

export inline auto throw_last_error(DWORD err = GetLastError())
{
	throw_error(HRESULT_FROM_WIN32(err));
}

auto get_volume_path(const std::filesystem::path& path)
{
	std::wstring buffer = std::filesystem::absolute(path);
	if (!GetVolumePathNameW(path.c_str(), buffer.data(), buffer.length()))
		throw_last_error();
	return std::filesystem::path(std::move(buffer));
}

auto get_volume_name(const std::filesystem::path& volume_path)
{
	std::wstring buffer;
	buffer.resize_and_overwrite(50, [&](WCHAR* buffer, std::size_t size)
	{
		if (!GetVolumeNameForVolumeMountPointW(volume_path.c_str(), buffer, size))
			throw_last_error();
		return std::wstring_view(buffer).length() - 1;
	});
	return std::filesystem::path(std::move(buffer));
}

auto open(const std::filesystem::path& path, long long chunk_size = 1, long long min_buffer_size = 2 * 1024 * 1024)
{
	std::clog << "opening " << path.string() << '\n' << std::flush;

	// TODO: use unique_handle
	const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, nullptr);

	if (file == INVALID_HANDLE_VALUE)
		throw_last_error();


	auto volume_path = get_volume_name(get_volume_path(path));

	std::clog << "   on " << volume_path.string() << '\n' << std::flush;

	const HANDLE volume = CreateFileW(volume_path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);

	if (volume == INVALID_HANDLE_VALUE)
		throw_last_error();

	STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR storage_alignment_desc;
	{
		STORAGE_PROPERTY_QUERY storage_query_property = {
			.PropertyId = StorageAccessAlignmentProperty
		};
		if (DWORD num_bytes_returned; !DeviceIoControl(volume, IOCTL_STORAGE_QUERY_PROPERTY, &storage_query_property, sizeof(storage_query_property), &storage_alignment_desc, sizeof(storage_alignment_desc), &num_bytes_returned, nullptr))
			throw_last_error();
	}

	std::clog << "   physical sector size " << storage_alignment_desc.BytesPerPhysicalSector << " B\n" << std::flush;

	LARGE_INTEGER file_size;
	if (!GetFileSizeEx(file, &file_size))
		throw_last_error();

	const long long min_read_size = std::lcm(chunk_size, storage_alignment_desc.BytesPerPhysicalSector);
	const long long read_size = (min_buffer_size + min_read_size - 1) / min_read_size * min_read_size;
	const long long buffer_alignment = std::lcm(storage_alignment_desc.BytesPerPhysicalSector, std::bit_ceil(storage_alignment_desc.BytesPerPhysicalSector));
	const long long buffer_size = (read_size + buffer_alignment - 1) / buffer_alignment * buffer_alignment;

	struct result { HANDLE file; long long file_size, read_size, buffer_size, buffer_alignment; };
	return result { file, file_size.QuadPart, read_size, buffer_size, buffer_alignment };
}

template <int num_buffers = 4> requires (num_buffers >= 2)
auto consume(const std::filesystem::path& path, long long chunk_size, auto&& sink)
{
	auto [file, file_size, read_size, buffer_size, buffer_alignment] = open(path, chunk_size);

	std::clog << "   using " << num_buffers << " buffers sized " << buffer_size << " B alignment " << buffer_alignment << " read size " << read_size << " B to read " << file_size << " B\n" << std::flush;

	auto buffer = static_cast<std::byte*>(::operator new(buffer_size * num_buffers, std::align_val_t(buffer_alignment)));

#if 1
	// IORING_CAPABILITIES caps;
	// QueryIoRingCapabilities(&caps);

	HIORING ioring;
	throw_error(CreateIoRing(IORING_VERSION_3, {}, 1024, 1024, &ioring));

	throw_error(BuildIoRingRegisterFileHandles(ioring, 1, &file, -1));

	IORING_BUFFER_INFO buffer_info = { buffer, static_cast<UINT32>(buffer_size * num_buffers) };
	throw_error(BuildIoRingRegisterBuffers(ioring, 1, &buffer_info, -2));

	throw_error(SubmitIoRing(ioring, 2, INFINITE, nullptr));
	{
		IORING_CQE cqe;
		throw_error(PopIoRingCompletion(ioring, &cqe));
		throw_error(cqe.ResultCode);
		throw_error(PopIoRingCompletion(ioring, &cqe));
		throw_error(cqe.ResultCode);
	}

	auto buffer_queue = [&]<int... I>(std::integer_sequence<int, I...>)
	{
		return io_buffer_queue<std::byte*, sizeof...(I)>(buffer + I * buffer_size...);
	}(std::make_integer_sequence<int, num_buffers>());

	long long read_offset = 0;
	long long bytes_read = 0;

	for (int i = 0; i < (num_buffers - 1) && read_offset < file_size; ++i)
	{
		auto next_buffer = buffer_queue.pop().release();
		throw_error(
			BuildIoRingReadFile(ioring,
			                    IoRingHandleRefFromIndex(0),
			                    IoRingBufferRefFromIndexAndOffset(0, next_buffer - buffer),
			                    read_size,
			                    read_offset,
			                    reinterpret_cast<UINT_PTR>(next_buffer),
			                    IOSQE_FLAGS_NONE));
		read_offset += read_size;
	}

	while (bytes_read != file_size)
	{
		throw_error(SubmitIoRing(ioring, 1, INFINITE, nullptr));

		IORING_CQE cqe;
		throw_error(PopIoRingCompletion(ioring, &cqe));

		if (cqe.ResultCode == HRESULT_FROM_WIN32(ERROR_HANDLE_EOF))
			continue;

		if (cqe.ResultCode != S_OK)
			throw_error(cqe.ResultCode);

		auto item = buffer_queue.reacquire(reinterpret_cast<std::byte*>(cqe.UserData));

		// sink(std::span<const std::byte>(buffer + i * buffer_size, cqe.Information), bytes_read, file_size);

		bytes_read += cqe.Information;

		if (read_offset < file_size)
		{
			auto next_buffer = buffer_queue.pop().release();
			throw_error(
				BuildIoRingReadFile(ioring,
				                    IoRingHandleRefFromIndex(0),
				                    IoRingBufferRefFromIndexAndOffset(0, next_buffer - buffer),
				                    read_size,
				                    read_offset,
				                    reinterpret_cast<UINT_PTR>(next_buffer),
				                    IOSQE_FLAGS_NONE));
			read_offset += read_size;
		}
	}
#else
	HANDLE iocp = CreateIoCompletionPort(file, 0, 0, 0);

	if (!iocp)
		throw_last_error();

	long long read_offset = 0;
	long long bytes_read = 0;

	OVERLAPPED o[num_buffers] = {};

	// auto res = SetFileIoOverlappedRange(file, reinterpret_cast<unsigned char*>(&o), sizeof(o));  // just an optimization, it's alright if this fails

	for (int i = 0; i < num_buffers && read_offset < file_size; ++i)
	{
		o[i] = {
			.Offset = static_cast<DWORD>(read_offset & 0xFFFFFFFF),
			.OffsetHigh = static_cast<DWORD>(read_offset >> 32),
		};

		ReadFile(file, buffer + i * buffer_size, read_size, nullptr, o + i);
		if (DWORD err = GetLastError(); err != ERROR_IO_PENDING)
			throw_last_error(err);
		read_offset += read_size;
	}

	auto read = [&, read_offset = std::atomic_ref<long long>(read_offset), bytes_read = std::atomic_ref<long long>(bytes_read)]
	{
		while (bytes_read.load(std::memory_order::relaxed) != file_size)
		{
			OVERLAPPED* po;
			DWORD bytes_transferred;
			ULONG_PTR key;
			if (!GetQueuedCompletionStatus(iocp, &bytes_transferred, &key, &po, INFINITE))
			{
				if (DWORD err = GetLastError(); err == ERROR_ABANDONED_WAIT_0)
					break;
				else
					throw_last_error(err);
			}

			if (bytes_read.fetch_add(bytes_transferred, std::memory_order::relaxed) + bytes_transferred == file_size)
				CloseHandle(iocp);

			auto i = ((po - o) + 1) % num_buffers;

			auto offset = (static_cast<long long>(po->OffsetHigh) << 32) | static_cast<long long>(po->Offset);

			sink(std::span<const std::byte>(buffer + i * buffer_size, bytes_transferred), offset);

			if (auto next_offset = read_offset.fetch_add(read_size, std::memory_order::relaxed); next_offset < file_size)
			{
				o[i] = {
					.Offset = static_cast<DWORD>(next_offset & 0xFFFFFFFF),
					.OffsetHigh = static_cast<DWORD>(next_offset >> 32),
				};

				ReadFile(file, buffer + i * buffer_size, read_size, nullptr, o + i);
				if (DWORD err = GetLastError(); err != ERROR_IO_PENDING)
					throw_last_error(err);
			}
		}
	};

	std::jthread threads[] = {
		std::jthread(read), std::jthread(read), std::jthread(read)
	};

	read();
#endif
	return bytes_read;
}

export auto consume_lines(const std::filesystem::path& path, auto&& sink)
{
	long long line = 1;
	std::string last_line;

	auto bytes_read = consume(path, 1, [&](std::span<const std::byte> bytes, long long offset, long long file_size)
	{
		std::clog << '\r';
		for (int i = 0; i < offset * 64 / file_size; ++i)
			std::clog << '.';
		std::clog << ' ' << (offset + bytes.size()) * 100 / file_size << "%";

		// std::string_view data(reinterpret_cast<const char*>(bytes.data()), bytes.size());

		// if (!last_line.empty())
		// {
		// 	auto prev_line_end = std::min(data.find('\n'), data.size());
		// 	last_line += data.substr(0, prev_line_end);
		// 	data.remove_prefix(prev_line_end);
		// 	sink(last_line, line);
		// 	last_line.clear();
		// }

		// for (;;)
		// {
		// 	auto skip_ws = [&](std::string_view data)
		// 	{
		// 		while (!data.empty())
		// 		{
		// 			switch (data[0])
		// 			{
		// 			case '\n':
		// 				++line;
		// 			case ' ':
		// 			case '\t':
		// 			case '\r':
		// 				break;

		// 			default:
		// 				return data;
		// 			}

		// 			data.remove_prefix(1);
		// 		}

		// 		return data;
		// 	};

		// 	data = skip_ws(data);

		// 	// auto line_start = std::min(data.find_first_not_of(" \t\r\n"), data.size());
		// 	// line += std::ranges::count(data.substr(0, line_start), '\n');
		// 	// data.remove_prefix(line_start);

		// 	if (data.empty())
		// 		break;


		// 	if (auto line_end = data.find('\n'); line_end != data.npos)
		// 	{
		// 		sink(data.substr(0, line_end), line);
		// 		data.remove_prefix(line_end);
		// 	}
		// 	else
		// 	{
		// 		last_line = data;
		// 		break;
		// 	}
		// }
	});

	std::clog << '\n';

	struct result { long long lines, bytes; };
	return result { line, bytes_read };
}
