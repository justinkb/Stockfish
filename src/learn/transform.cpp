#include "transform.h"

#include "sfen_stream.h"
#include "packed_sfen.h"

#include "thread.h"
#include "position.h"
#include "evaluate.h"
#include "search.h"

#include "nnue/evaluate_nnue.h"

#include <string>
#include <map>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

namespace Learner
{
    using CommandFunc = void(*)(std::istringstream&);

    enum struct NudgedStaticMode
    {
        Absolute,
        Relative,
        Interpolate
    };

    struct NudgedStaticParams
    {
        std::string input_filename = "in.binpack";
        std::string output_filename = "out.binpack";
        NudgedStaticMode mode = NudgedStaticMode::Absolute;
        int absolute_nudge = 5;
        float relative_nudge = 0.1;
        float interpolate_nudge = 0.1;

        void enforce_constraints()
        {
            relative_nudge = std::max(relative_nudge, 0.0f);
            absolute_nudge = std::max(absolute_nudge, 0);
        }
    };

    struct RescoreFenParams
    {
        std::string input_filename = "in.epd";
        std::string output_filename = "out.binpack";
        int depth = 3;

        void enforce_constraints()
        {
            depth = std::max(1, depth);
        }
    };

    [[nodiscard]] std::int16_t nudge(NudgedStaticParams& params, std::int16_t static_eval_i16, std::int16_t deep_eval_i16)
    {
        auto saturate_i32_to_i16 = [](int v) {
            return static_cast<std::int16_t>(
                std::clamp(
                    v,
                    (int)std::numeric_limits<std::int16_t>::min(),
                    (int)std::numeric_limits<std::int16_t>::max()
                )
            );
        };

        auto saturate_f32_to_i16 = [saturate_i32_to_i16](float v) {
            return saturate_i32_to_i16((int)v);
        };

        int static_eval = static_eval_i16;
        int deep_eval = deep_eval_i16;

        switch(params.mode)
        {
            case NudgedStaticMode::Absolute:
                return saturate_i32_to_i16(
                    static_eval + std::clamp(
                        deep_eval - static_eval,
                        -params.absolute_nudge,
                        params.absolute_nudge
                    )
                );

            case NudgedStaticMode::Relative:
                return saturate_f32_to_i16(
                    (float)static_eval * std::clamp(
                        (float)deep_eval / (float)static_eval,
                        (1.0f - params.relative_nudge),
                        (1.0f + params.relative_nudge)
                    )
                );

            case NudgedStaticMode::Interpolate:
                return saturate_f32_to_i16(
                    (float)static_eval * (1.0f - params.interpolate_nudge)
                    + (float)deep_eval * params.interpolate_nudge
                );

            default:
                assert(false);
                return 0;
        }
    }

    void do_nudged_static(NudgedStaticParams& params)
    {
        Thread* th = Threads.main();
        Position& pos = th->rootPos;
        StateInfo si;

        auto in = Learner::open_sfen_input_file(params.input_filename);
        auto out = Learner::create_new_sfen_output(params.output_filename);

        if (in == nullptr)
        {
            std::cerr << "Invalid input file type.\n";
            return;
        }

        if (out == nullptr)
        {
            std::cerr << "Invalid output file type.\n";
            return;
        }

        PSVector buffer;
        uint64_t batch_size = 1'000'000;

        buffer.reserve(batch_size);

        uint64_t num_processed = 0;
        for (;;)
        {
            auto v = in->next();
            if (!v.has_value())
                break;

            auto& ps = v.value();

            pos.set_from_packed_sfen(ps.sfen, &si, th);
            auto static_eval = Eval::evaluate(pos);
            auto deep_eval = ps.score;
            ps.score = nudge(params, static_eval, deep_eval);

            buffer.emplace_back(ps);
            if (buffer.size() >= batch_size)
            {
                num_processed += buffer.size();

                out->write(buffer);
                buffer.clear();

                std::cout << "Processed " << num_processed << " positions.\n";
            }
        }

        if (!buffer.empty())
        {
            num_processed += buffer.size();

            out->write(buffer);
            buffer.clear();

            std::cout << "Processed " << num_processed << " positions.\n";
        }

        std::cout << "Finished.\n";
    }

    void nudged_static(std::istringstream& is)
    {
        NudgedStaticParams params{};

        while(true)
        {
            std::string token;
            is >> token;

            if (token == "")
                break;

            if (token == "absolute")
            {
                params.mode = NudgedStaticMode::Absolute;
                is >> params.absolute_nudge;
            }
            else if (token == "relative")
            {
                params.mode = NudgedStaticMode::Relative;
                is >> params.relative_nudge;
            }
            else if (token == "interpolate")
            {
                params.mode = NudgedStaticMode::Interpolate;
                is >> params.interpolate_nudge;
            }
            else if (token == "input_file")
                is >> params.input_filename;
            else if (token == "output_file")
                is >> params.output_filename;
        }

        std::cout << "Performing transform nudged_static with parameters:\n";
        std::cout << "input_file          : " << params.input_filename << '\n';
        std::cout << "output_file         : " << params.output_filename << '\n';
        std::cout << "\n";
        if (params.mode == NudgedStaticMode::Absolute)
        {
            std::cout << "mode                : absolute\n";
            std::cout << "absolute_nudge      : " << params.absolute_nudge << '\n';
        }
        else if (params.mode == NudgedStaticMode::Relative)
        {
            std::cout << "mode                : relative\n";
            std::cout << "relative_nudge      : " << params.relative_nudge << '\n';
        }
        else if (params.mode == NudgedStaticMode::Interpolate)
        {
            std::cout << "mode                : interpolate\n";
            std::cout << "interpolate_nudge   : " << params.interpolate_nudge << '\n';
        }
        std::cout << '\n';

        params.enforce_constraints();
        do_nudged_static(params);
    }

    void do_rescore_fen(RescoreFenParams& params)
    {
        std::ifstream fens_file(params.input_filename);

        auto next_fen = [&fens_file]() -> std::optional<std::string>{
            static std::mutex mutex;

            std::string fen;

            std::unique_lock lock(mutex);

            if (std::getline(fens_file, fen) && fen.size() >= 10)
            {
                return fen;
            }
            else
            {
                return std::nullopt;
            }
        };

        PSVector buffer;
        uint64_t batch_size = 10'000;

        buffer.reserve(batch_size);

        auto out = Learner::create_new_sfen_output(params.output_filename);

        std::mutex mutex;
        uint64_t num_processed = 0;

        // About Search::Limits
        // Be careful because this member variable is global and affects other threads.
        auto& limits = Search::Limits;

        // Make the search equivalent to the "go infinite" command. (Because it is troublesome if time management is done)
        limits.infinite = true;

        // Since PV is an obstacle when displayed, erase it.
        limits.silent = true;

        // If you use this, it will be compared with the accumulated nodes of each thread. Therefore, do not use it.
        limits.nodes = 0;

        // depth is also processed by the one passed as an argument of Learner::search().
        limits.depth = 0;

        Threads.execute_with_workers([&](auto& th){
            Position& pos = th.rootPos;
            StateInfo si;

            for(;;)
            {
                auto fen = next_fen();
                if (!fen.has_value())
                    return;

                pos.set(*fen, false, &si, &th);
                pos.state()->rule50 = 0;

                auto [search_value, search_pv] = Search::search(pos, params.depth, 1);
                if (search_pv.empty())
                    continue;

                PackedSfenValue ps;
                pos.sfen_pack(ps.sfen);
                ps.score = search_value;
                ps.move = search_pv[0];
                ps.gamePly = 1;
                ps.game_result = 0;
                ps.padding = 0;

                std::unique_lock lock(mutex);
                buffer.emplace_back(ps);
                if (buffer.size() >= batch_size)
                {
                    num_processed += buffer.size();

                    out->write(buffer);
                    buffer.clear();

                    std::cout << "Processed " << num_processed << " positions.\n";
                }
            }
        });
        Threads.wait_for_workers_finished();

        if (!buffer.empty())
        {
            num_processed += buffer.size();

            out->write(buffer);
            buffer.clear();

            std::cout << "Processed " << num_processed << " positions.\n";
        }

        std::cout << "Finished.\n";
    }

    void rescore_fen(std::istringstream& is)
    {
        RescoreFenParams params{};

        while(true)
        {
            std::string token;
            is >> token;

            if (token == "")
                break;

            if (token == "depth")
                is >> params.depth;
            else if (token == "input_file")
                is >> params.input_filename;
            else if (token == "output_file")
                is >> params.output_filename;
        }

        std::cout << "Performing transform rescore_fen with parameters:\n";
        std::cout << "depth               : " << params.depth << '\n';
        std::cout << "input_file          : " << params.input_filename << '\n';
        std::cout << "output_file         : " << params.output_filename << '\n';
        std::cout << '\n';

        params.enforce_constraints();
        do_rescore_fen(params);
    }

    void transform(std::istringstream& is)
    {
        const std::map<std::string, CommandFunc> subcommands = {
            { "nudged_static", &nudged_static },
            { "rescore_fen", &rescore_fen }
        };

        Eval::NNUE::init();

        std::string subcommand;
        is >> subcommand;

        auto func = subcommands.find(subcommand);
        if (func == subcommands.end())
        {
            std::cout << "Invalid subcommand " << subcommand << ". Exiting...\n";
            return;
        }

        func->second(is);
    }

}