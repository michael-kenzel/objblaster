#include <stdexcept>
#include <limits>
#include <cstddef>
#include <cmath>
#include <string_view>
#include <string>
#include <vector>
#include <algorithm>
#include <numeric>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <fstream>

import io;

using namespace std::literals;


namespace
{
	auto parse_int(std::string_view& str)
	{
		long long value;
		auto [ptr, err] = std::from_chars(str.data(), str.data() + str.size(), value);
		str.remove_prefix(ptr - str.data());
		return value;
	}

	auto parse_float(std::string_view& str)
	{
		float value;
		auto [ptr, err] = std::from_chars(str.data(), str.data() + str.size(), value);
		str.remove_prefix(ptr - str.data());
		return value;
	}

	struct vector
	{
		float x, y, z;
	};

	constexpr vector operator -(vector v)
	{
		return { -v.x, -v.y, -v.z };
	}

	vector parse_vector(std::string_view str)
	{
		float x = parse_float(str);
		str.remove_prefix(1);
		float y = parse_float(str);
		str.remove_prefix(1);
		float z = parse_float(str);
		return { x, y, z };
	}

	struct face
	{
		long long v[3];
		long long vt[3];
		long long vn[3];
	};
}

int main(int argc, char** argv)
{
	try
	{
		if (argc != 2)
		{
			std::cerr << "usage: splitobj <file name>\n";
			return -1;
		}

		auto path = std::filesystem::path(argv[1]);

		auto start = std::chrono::steady_clock::now();

		vector min = { std::numeric_limits<float>::min(), std::numeric_limits<float>::min(), std::numeric_limits<float>::min() };
		vector max = -min;

		std::vector<vector> v;
		std::vector<vector> vt;
		std::vector<vector> vn;
		std::vector<face> f;
		// long long faces = 0;


		auto parse_vertex = [&](std::string_view line)
		{
			line.remove_prefix(1);

			switch (line[0])
			{
			case ' ':
				{
					line.remove_prefix(1);

					auto p = parse_vector(line);

					min.x = std::min(min.x, p.x);
					min.y = std::min(min.y, p.y);
					min.z = std::min(min.z, p.z);

					max.x = std::max(max.x, p.x);
					max.y = std::max(max.y, p.y);
					max.z = std::max(max.z, p.z);

					v.push_back(p);
				}
				break;

			case 't':
				{
					line.remove_prefix(2);
					auto t = parse_vector(line);
					vt.push_back(t);
				}
				break;

			case 'n':
				{
					line.remove_prefix(2);
					auto n = parse_vector(line);
					vn.push_back(n);
				}
				break;
			}
		};

		auto parse_face = [&](std::string_view line)
		{
			line.remove_prefix(1);

			face face;

			for (int i = 0; i < 3; ++i)
			{
				line.remove_prefix(1);

				face.v[i] = parse_int(line);
				face.v[i] += face.v[i] < 0 ? v.size() : -1;

				if (line[0] == '/')
				{
					line.remove_prefix(1);

					if (line[0] != '/')
					{
						face.vt[i] = parse_int(line);
						face.vt[i] += face.vt[i] < 0 ? vt.size() : -1;
					}
					else
						face.vt[i] = -1;
				}

				if (line[0] == '/')
				{
					line.remove_prefix(1);
					face.vn[i] = parse_int(line);
					face.vn[i] += face.vn[i] < 0 ? vn.size() : -1;
				}
				else
					face.vn[i] = -1;

				f.push_back(face);
			}
		};

		auto [lines_read, bytes_read] = consume_lines(path, [&](std::string_view line, long long line_number)
		{
			switch (line[0])
			{
			case 'v':
				parse_vertex(line);
				break;

			case 'f':
				parse_face(line);
				break;

			case '#':
			case 'm':
			case 'o':
			case 'g':
			case 'u':
				break;

			default:
				throw std::runtime_error("line "s + std::to_string(line_number) + ": unknown statement '"s + std::string(line) + '\'');
			}
		});

		auto end = std::chrono::steady_clock::now();

		auto t = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

		std::clog << "read " << lines_read << " lines, " << bytes_read / (1024.0 * 1024.0 * 1024.0) << " GiB in " << t * 0.001 << " s (" << bytes_read * 1000.0 / (1024.0 * 1024.0 * 1024.0 * t) << " GiB/s)\n";
		std::cout << f.size() << " faces\n"
		          << v.size() << " vertices\n"
		          << vt.size() << " texcoords\n"
		          << vn.size() << " normals\n";
		std::cout << "bounding box: " << (max.x - min.x) << " x " << (max.y - min.y) << " x " << (max.z - min.z) << "\n\tmin = ("
		                              << min.x << ", " << min.y << ", " << min.z << ")\n\tmax = ("
		                              << max.x << ", " << max.y << ", " << max.z << ")\n";
	}
	catch (const win32error& e)
	{
		std::cerr << "ERROR 0x" << std::hex << e.error_code << '\n';
		return -1;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ERROR: " << e.what() << '\n';
		return -1;
	}
	catch (...)
	{
		std::cerr << "ERROR: unknown exception\n";
		return -128;
	}

	return 0;
}
