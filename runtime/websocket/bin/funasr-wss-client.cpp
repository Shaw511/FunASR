/**
 * Copyright FunASR (https://github.com/alibaba-damo-academy/FunASR). All Rights
 * Reserved. MIT License  (https://opensource.org/licenses/MIT)
 */
/* 2022-2023 by zhaomingwork */

// client for websocket, support multiple threads
// ./funasr-wss-client  --server-ip <string>
//                     --port <string>
//                     --wav-path <string>
//                     [--thread-num <int>] 
//                     [--is-ssl <int>]  [--]
//                     [--version] [-h]
// example:
// ./funasr-wss-client --server-ip 127.0.0.1 --port 10095 --wav-path test.wav --thread-num 1 --is-ssl 1

#define ASIO_STANDALONE 1
#include <websocketpp/client.hpp>
#include <websocketpp/common/thread.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <fstream>
#include <atomic>
#include <thread>
#include <glog/logging.h>
#include "util.h"
#include "audio.h"
#include "nlohmann/json.hpp"
#include "tclap/CmdLine.h"
#include "microphone.h" // Include the microphone header

/**
 * Define a semi-cross platform helper method that waits/sleeps for a bit.
 */
void WaitABit() {
    #ifdef WIN32
        Sleep(200);
    #else
        usleep(200);
    #endif
}
std::atomic<int> wav_index(0);

typedef websocketpp::config::asio_client::message_type::ptr message_ptr;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context> context_ptr;
using websocketpp::lib::bind;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

context_ptr OnTlsInit(websocketpp::connection_hdl) {
    context_ptr ctx = websocketpp::lib::make_shared<asio::ssl::context>(
        asio::ssl::context::sslv23);

    try {
        ctx->set_options(
            asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
            asio::ssl::context::no_sslv3 | asio::ssl::context::single_dh_use);

    } catch (std::exception& e) {
        LOG(ERROR) << e.what();
    }
    return ctx;
}

// template for tls or not config
template <typename T>
class WebsocketClient {
  public:
    typedef websocketpp::lib::lock_guard<websocketpp::lib::mutex> scoped_lock;

    WebsocketClient(int is_ssl) : m_open(false), m_done(false) {
        m_client.clear_access_channels(websocketpp::log::alevel::all);
        m_client.set_access_channels(websocketpp::log::alevel::connect);
        m_client.set_access_channels(websocketpp::log::alevel::disconnect);
        m_client.set_access_channels(websocketpp::log::alevel::app);

        m_client.init_asio();

        using websocketpp::lib::bind;
        m_client.set_open_handler(bind(&WebsocketClient::on_open, this, _1));
        m_client.set_close_handler(bind(&WebsocketClient::on_close, this, _1));

        m_client.set_message_handler(
            [this](websocketpp::connection_hdl hdl, message_ptr msg) {
              on_message(hdl, msg);
            });

        m_client.set_fail_handler(bind(&WebsocketClient::on_fail, this, _1));
        m_client.clear_access_channels(websocketpp::log::alevel::all);
    }

    void on_message(websocketpp::connection_hdl hdl, message_ptr msg) {
        const std::string& payload = msg->get_payload();
        switch (msg->get_opcode()) {
            case websocketpp::frame::opcode::text:
                LOG(INFO) << "Thread: " << this_thread::get_id() << ", on_message = " << payload;
                break;
        }
    }

    void run(const std::string& uri, const std::vector<string>& wav_list, const std::vector<string>& wav_ids,
             int audio_fs, const std::unordered_map<std::string, int>& hws_map, int use_itn=1) {
        websocketpp::lib::error_code ec;
        typename websocketpp::client<T>::connection_ptr con = m_client.get_connection(uri, ec);
        if (ec) {
            m_client.get_alog().write(websocketpp::log::alevel::app, "Get Connection Error: " + ec.message());
            return;
        }
        m_hdl = con->get_handle();
        m_client.connect(con);

        websocketpp::lib::thread asio_thread(&websocketpp::client<T>::run, &m_client);

        // Start sending recorded data
        send_rec_data(audio_fs, hws_map, use_itn);

        asio_thread.join();
    }

    void send_rec_data(int audio_fs, const std::unordered_map<std::string, int>& hws_map, int use_itn) {
        bool wait = false;
        while (1) {
            {
                scoped_lock guard(m_lock);
                if (m_done) {
                    break;
                }
                if (!m_open) {
                    wait = true;
                } else {
                    break;
                }
            }

            if (wait) {
                WaitABit();
                continue;
            }
        }
        websocketpp::lib::error_code ec;

        float sample_rate = static_cast<float>(audio_fs);
        nlohmann::json jsonbegin;
        jsonbegin["mode"] = "record";
        jsonbegin["wav_name"] = "record";
        jsonbegin["wav_format"] = "pcm";
        jsonbegin["audio_fs"] = sample_rate;
        jsonbegin["is_speaking"] = true;
        jsonbegin["itn"] = use_itn == 1;

        if (!hws_map.empty()) {
            nlohmann::json json_map(hws_map);
            jsonbegin["hotwords"] = json_map.dump();
        }
        m_client.send(m_hdl, jsonbegin.dump(), websocketpp::frame::opcode::text, ec);

        // Microphone setup
        Microphone mic;
        PaStream* stream;
        PaError err = Pa_OpenDefaultStream(&stream, 1, 0, paFloat32, sample_rate, 256, RecordCallback, this);
        if (err != paNoError) {
            LOG(ERROR) << "PortAudio error: " << Pa_GetErrorText(err);
            return;
        }

        err = Pa_StartStream(stream);
        if (err != paNoError) {
            LOG(ERROR) << "PortAudio error: " << Pa_GetErrorText(err);
            return;
        }

        while (true) {
            // [Add the code to read from the microphone and send data to the server]
            // This part should implement the logic to send recorded audio data.
        }

        jsonbegin["is_speaking"] = false;
        m_client.send(m_hdl, jsonbegin.dump(), websocketpp::frame::opcode::text, ec);

        err = Pa_CloseStream(stream);
        if (err != paNoError) {
            LOG(ERROR) << "PortAudio error: " << Pa_GetErrorText(err);
        }
    }

    static int RecordCallback(const void* inputBuffer, void* outputBuffer,
                              unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo,
                              PaStreamCallbackFlags statusFlags, void* userData) {
        // Implement your logic to handle the inputBuffer and send it to the WebSocket
        return paContinue;
    }

    websocketpp::client<T> m_client;

  private:
    websocketpp::connection_hdl m_hdl;
    websocketpp::lib::mutex m_lock;
    bool m_open;
    bool m_done;
};

int main(int argc, char* argv[]) {
#ifdef _WIN32
    #include <windows.h>
    SetConsoleOutputCP(65001);
#endif
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;

    TCLAP::CmdLine cmd("funasr-wss-client", ' ', "1.0");
    TCLAP::ValueArg<std::string> server_ip_("", "server-ip", "server-ip", true, "127.0.0.1", "string");
    TCLAP::ValueArg<std::string> port_("", "port", "port", true, "10095", "string");
    TCLAP::ValueArg<std::int32_t> audio_fs_("", "audio-fs", "the sample rate of audio", false, 16000, "int32_t");
    TCLAP::ValueArg<int> thread_num_("", "thread-num", "thread-num", false, 1, "int");
    TCLAP::ValueArg<int> is_ssl_("", "is-ssl", "is-ssl is 1 means use wss connection, or use ws connection", false, 1, "int");
    TCLAP::ValueArg<int> use_itn_("", "use-itn", "use-itn is 1 means use itn, 0 means not use itn", false, 1, "int");

    cmd.add(server_ip_);
    cmd.add(port_);
    cmd.add(audio_fs_);
    cmd.add(thread_num_);
    cmd.add(is_ssl_);
    cmd.add(use_itn_);
    cmd.parse(argc, argv);

    std::string server_ip = server_ip_.getValue();
    std::string port = port_.getValue();
    int threads_num = thread_num_.getValue();
    int is_ssl = is_ssl_.getValue();
    int use_itn = use_itn_.getValue();

    std::string uri = (is_ssl == 1) ? "wss://" + server_ip + ":" + port : "ws://" + server_ip + ":" + port;

    for (size_t i = 0; i < threads_num; i++) {
        std::thread([uri, audio_fs = audio_fs_.getValue(), is_ssl, use_itn]() {
            if (is_ssl == 1) {
                WebsocketClient<websocketpp::config::asio_tls_client> c(is_ssl);
                c.m_client.set_tls_init_handler(bind(&OnTlsInit, ::_1));
                c.run(uri, {}, {}, audio_fs, {}, use_itn);
            } else {
                WebsocketClient<websocketpp::config::asio_client> c(is_ssl);
                c.run(uri, {}, {}, audio_fs, {}, use_itn);
            }
        }).detach();
    }
}
