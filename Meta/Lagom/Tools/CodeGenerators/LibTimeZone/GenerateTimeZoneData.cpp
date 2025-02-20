/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@pm.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../LibUnicode/GeneratorUtil.h" // FIXME: Move this somewhere common.
#include <AK/Format.h>
#include <AK/HashMap.h>
#include <AK/SourceGenerator.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>

namespace {

struct DateTime {
    u16 year { 0 };
    Optional<u8> month;
    Optional<u8> day;

    Optional<u8> last_weekday;
    Optional<u8> after_weekday;

    Optional<u8> hour;
    Optional<u8> minute;
    Optional<u8> second;
};

struct TimeZoneOffset {
    i64 offset { 0 };
    Optional<DateTime> until;
};

struct TimeZoneData {
    HashMap<String, Vector<TimeZoneOffset>> time_zones;
    Vector<String> time_zone_names;
    Vector<Alias> time_zone_aliases;
};

}

template<>
struct AK::Formatter<DateTime> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, DateTime const& date_time)
    {
        return Formatter<FormatString>::format(builder,
            "{{ {}, {}, {}, {}, {}, {}, {}, {} }}",
            date_time.year,
            date_time.month.value_or(1),
            date_time.day.value_or(1),
            date_time.last_weekday.value_or(0),
            date_time.after_weekday.value_or(0),
            date_time.hour.value_or(0),
            date_time.minute.value_or(0),
            date_time.second.value_or(0));
    }
};

template<>
struct AK::Formatter<TimeZoneOffset> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, TimeZoneOffset const& time_zone_offset)
    {
        return Formatter<FormatString>::format(builder,
            "{{ {}, {}, {} }}",
            time_zone_offset.offset,
            time_zone_offset.until.value_or({}),
            time_zone_offset.until.has_value());
    }
};

static Optional<DateTime> parse_date_time(Span<StringView const> segments)
{
    constexpr auto months = Array { "Jan"sv, "Feb"sv, "Mar"sv, "Apr"sv, "May"sv, "Jun"sv, "Jul"sv, "Aug"sv, "Sep"sv, "Oct"sv, "Nov"sv, "Dec"sv };
    constexpr auto weekdays = Array { "Sun"sv, "Mon"sv, "Tue"sv, "Wed"sv, "Thu"sv, "Fri"sv, "Sat"sv };

    auto comment_index = find_index(segments.begin(), segments.end(), "#"sv);
    if (comment_index != segments.size())
        segments = segments.slice(0, comment_index);
    if (segments.is_empty())
        return {};

    DateTime date_time {};
    date_time.year = segments[0].to_uint().value();

    if (segments.size() > 1)
        date_time.month = find_index(months.begin(), months.end(), segments[1]) + 1;

    if (segments.size() > 2) {
        if (segments[2].starts_with("last"sv)) {
            auto weekday = segments[2].substring_view("last"sv.length());
            date_time.last_weekday = find_index(weekdays.begin(), weekdays.end(), weekday);
        } else if (auto index = segments[2].find(">="sv); index.has_value()) {
            auto weekday = segments[2].substring_view(0, *index);
            date_time.after_weekday = find_index(weekdays.begin(), weekdays.end(), weekday);

            auto day = segments[2].substring_view(*index + ">="sv.length());
            date_time.day = day.to_uint().value();
        } else {
            date_time.day = segments[2].to_uint().value();
        }
    }

    if (segments.size() > 3) {
        // FIXME: Some times end with a letter, e.g. "2:00u" and "2:00s". Figure out what this means and handle it.
        auto time_segments = segments[3].split_view(':');

        date_time.hour = time_segments[0].to_int().value();
        date_time.minute = time_segments.size() > 1 ? time_segments[1].substring_view(0, 2).to_uint().value() : 0;
        date_time.second = time_segments.size() > 2 ? time_segments[2].substring_view(0, 2).to_uint().value() : 0;
    }

    return date_time;
}

static i64 parse_time_offset(StringView segment)
{
    auto segments = segment.split_view(':');

    i64 hours = segments[0].to_int().value();
    i64 minutes = segments.size() > 1 ? segments[1].to_uint().value() : 0;
    i64 seconds = segments.size() > 2 ? segments[2].to_uint().value() : 0;

    i64 sign = ((hours < 0) || (segments[0] == "-0"sv)) ? -1 : 1;
    return (hours * 3600) + sign * ((minutes * 60) + seconds);
}

static Vector<TimeZoneOffset>& parse_zone(StringView zone_line, TimeZoneData& time_zone_data)
{
    auto segments = zone_line.split_view_if([](char ch) { return (ch == '\t') || (ch == ' '); });

    // "Zone" NAME STDOFF RULES FORMAT [UNTIL]
    VERIFY(segments[0] == "Zone"sv);
    auto name = segments[1];

    TimeZoneOffset time_zone {};
    time_zone.offset = parse_time_offset(segments[2]);

    if (segments.size() > 5)
        time_zone.until = parse_date_time(segments.span().slice(5));

    auto& time_zones = time_zone_data.time_zones.ensure(name);
    time_zones.append(move(time_zone));

    if (!time_zone_data.time_zone_names.contains_slow(name))
        time_zone_data.time_zone_names.append(name);

    return time_zones;
}

static void parse_zone_continuation(StringView zone_line, Vector<TimeZoneOffset>& time_zones)
{
    auto segments = zone_line.split_view_if([](char ch) { return (ch == '\t') || (ch == ' '); });

    // STDOFF RULES FORMAT [UNTIL]
    TimeZoneOffset time_zone {};
    time_zone.offset = parse_time_offset(segments[0]);

    if (segments.size() > 3)
        time_zone.until = parse_date_time(segments.span().slice(3));

    time_zones.append(move(time_zone));
}

static void parse_link(StringView link_line, TimeZoneData& time_zone_data)
{
    auto segments = link_line.split_view_if([](char ch) { return (ch == '\t') || (ch == ' '); });

    // Link TARGET LINK-NAME
    VERIFY(segments[0] == "Link"sv);
    auto target = segments[1];
    auto alias = segments[2];

    time_zone_data.time_zone_aliases.append({ target, alias });
}

static ErrorOr<void> parse_time_zones(StringView time_zone_path, TimeZoneData& time_zone_data)
{
    // For reference, the man page for `zic` has the best documentation of the TZDB file format.
    auto file = TRY(Core::File::open(time_zone_path, Core::OpenMode::ReadOnly));
    Vector<TimeZoneOffset>* last_parsed_zone = nullptr;

    while (file->can_read_line()) {
        auto line = file->read_line();
        if (line.is_empty() || line.trim_whitespace(TrimMode::Left).starts_with('#'))
            continue;

        if (line.starts_with("Zone"sv)) {
            last_parsed_zone = &parse_zone(line, time_zone_data);
        } else if (line.starts_with('\t')) {
            VERIFY(last_parsed_zone != nullptr);
            parse_zone_continuation(line, *last_parsed_zone);
        } else {
            last_parsed_zone = nullptr;

            if (line.starts_with("Link"sv))
                parse_link(line, time_zone_data);
        }
    }

    return {};
}

static String format_identifier(StringView owner, String identifier)
{
    constexpr auto gmt_time_zones = Array { "Etc/GMT"sv, "GMT"sv };

    for (auto gmt_time_zone : gmt_time_zones) {
        if (identifier.starts_with(gmt_time_zone)) {
            auto offset = identifier.substring_view(gmt_time_zone.length());

            if (offset.starts_with('+'))
                identifier = String::formatted("{}_Ahead_{}", gmt_time_zone, offset.substring_view(1));
            else if (offset.starts_with('-'))
                identifier = String::formatted("{}_Behind_{}", gmt_time_zone, offset.substring_view(1));
        }
    }

    identifier = identifier.replace("-"sv, "_"sv, true);
    identifier = identifier.replace("/"sv, "_"sv, true);

    if (all_of(identifier, is_ascii_digit))
        return String::formatted("{}_{}", owner[0], identifier);
    if (is_ascii_lower_alpha(identifier[0]))
        return String::formatted("{:c}{}", to_ascii_uppercase(identifier[0]), identifier.substring_view(1));
    return identifier;
}

static void generate_time_zone_data_header(Core::File& file, TimeZoneData& time_zone_data)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#pragma once

#include <AK/Types.h>

namespace TimeZone {
)~~~");

    generate_enum(generator, format_identifier, "TimeZone"sv, {}, time_zone_data.time_zone_names, time_zone_data.time_zone_aliases);

    generator.append(R"~~~(
}
)~~~");

    VERIFY(file.write(generator.as_string_view()));
}

static void generate_time_zone_data_implementation(Core::File& file, TimeZoneData& time_zone_data)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#include <AK/Array.h>
#include <AK/BinarySearch.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <LibTimeZone/TimeZone.h>
#include <LibTimeZone/TimeZoneData.h>

namespace TimeZone {

struct DateTime {
    AK::Time time_since_epoch() const
    {
        // FIXME: This implementation does not take last_weekday or after_weekday into account.
        return AK::Time::from_timestamp(year, month, day, hour, minute, second, 0);
    }

    u16 year { 0 };
    u8 month { 1 };
    u8 day { 1 };

    u8 last_weekday { 0 };
    u8 after_weekday { 0 };

    u8 hour { 0 };
    u8 minute { 0 };
    u8 second { 0 };
};

struct TimeZoneOffset {
    i64 offset { 0 };

    DateTime until {};
    bool has_until { false };
};
)~~~");

    auto append_time_zone_offsets = [&](auto const& name, auto const& time_zone_offsets) {
        generator.set("name", name);
        generator.set("size", String::number(time_zone_offsets.size()));

        generator.append(R"~~~(
static constexpr Array<TimeZoneOffset, @size@> @name@ { {
)~~~");

        for (auto const& time_zone_offset : time_zone_offsets)
            generator.append(String::formatted("    {},\n", time_zone_offset));

        generator.append("} };\n");
    };

    generate_mapping(generator, time_zone_data.time_zone_names, "TimeZoneOffset"sv, "s_time_zone_offsets"sv, "s_time_zone_offsets_{}", format_identifier,
        [&](auto const& name, auto const& value) {
            auto const& time_zone_offsets = time_zone_data.time_zones.find(value)->value;
            append_time_zone_offsets(name, time_zone_offsets);
        });

    auto append_string_conversions = [&](StringView enum_title, StringView enum_snake, auto const& values, auto const& aliases) {
        HashValueMap<String> hashes;
        hashes.ensure_capacity(values.size());

        auto hash = [](auto const& value) {
            return CaseInsensitiveStringViewTraits::hash(value);
        };

        for (auto const& value : values)
            hashes.set(hash(value), format_identifier(enum_title, value));
        for (auto const& alias : aliases)
            hashes.set(hash(alias.alias), format_identifier(enum_title, alias.alias));

        ValueFromStringOptions options {};
        options.sensitivity = CaseSensitivity::CaseInsensitive;

        generate_value_from_string(generator, "{}_from_string"sv, enum_title, enum_snake, move(hashes), options);
        generate_value_to_string(generator, "{}_to_string"sv, enum_title, enum_snake, format_identifier, values);
    };

    append_string_conversions("TimeZone"sv, "time_zone"sv, time_zone_data.time_zone_names, time_zone_data.time_zone_aliases);

    generator.append(R"~~~(
Optional<i64> get_time_zone_offset(TimeZone time_zone, AK::Time time)
{
    // FIXME: This implementation completely ignores DST.
    auto const& time_zone_offsets = s_time_zone_offsets[to_underlying(time_zone)];

    size_t index = 0;
    for (; index < time_zone_offsets.size(); ++index) {
        auto const& time_zone_offset = time_zone_offsets[index];

        if (!time_zone_offset.has_until || (time_zone_offset.until.time_since_epoch() > time))
            break;
    }

    VERIFY(index < time_zone_offsets.size());
    return time_zone_offsets[index].offset;
}

}
)~~~");

    VERIFY(file.write(generator.as_string_view()));
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    Vector<StringView> time_zone_paths;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the time zone data header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the time zone data implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_positional_argument(time_zone_paths, "Paths to the time zone database files", "time-zone-paths");
    args_parser.parse(arguments);

    auto open_file = [&](StringView path) -> ErrorOr<NonnullRefPtr<Core::File>> {
        if (path.is_empty()) {
            args_parser.print_usage(stderr, arguments.argv[0]);
            return Error::from_string_literal("Must provide all command line options"sv);
        }

        return Core::File::open(path, Core::OpenMode::ReadWrite);
    };

    auto generated_header_file = TRY(open_file(generated_header_path));
    auto generated_implementation_file = TRY(open_file(generated_implementation_path));

    TimeZoneData time_zone_data {};
    for (auto time_zone_path : time_zone_paths)
        TRY(parse_time_zones(time_zone_path, time_zone_data));

    generate_time_zone_data_header(generated_header_file, time_zone_data);
    generate_time_zone_data_implementation(generated_implementation_file, time_zone_data);

    return 0;
}
