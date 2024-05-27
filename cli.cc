#include <boost/asio.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/string.hpp>
#include <iostream>
#include <sstream>
#include <vector>
#include <mutex>
#include <thread>
#include <chrono>
#include <memory>

class Data
{
public:
    int id;
    std::string content;

    template<class Archive>
    void serialize(Archive & ar, const unsigned int version)
    {
        ar & id;
        ar & content;
    }
};

class connection
{
public:
    connection(boost::asio::io_service& io_service)
        : socket_(io_service)
    {
    }

    boost::asio::ip::tcp::socket& socket()
    {
        return socket_;
    }

    template <typename T, typename Handler>
    void async_write(const T& t, Handler handler)
    {
        std::ostringstream archive_stream;
        boost::archive::text_oarchive archive(archive_stream);
        archive << t;
        outbound_data_ = archive_stream.str();

        std::ostringstream header_stream;
        header_stream << std::setw(header_length)
                      << std::hex << outbound_data_.size();
        if (!header_stream || header_stream.str().size() != header_length)
        {
            boost::system::error_code error(boost::asio::error::invalid_argument);
            socket_.get_io_service().post(boost::bind(handler, error));
            return;
        }
        outbound_header_ = header_stream.str();

        std::vector<boost::asio::const_buffer> buffers;
        buffers.push_back(boost::asio::buffer(outbound_header_));
        buffers.push_back(boost::asio::buffer(outbound_data_));
        boost::asio::async_write(socket_, buffers, handler);
    }

    template <typename T, typename Handler>
    void async_read(T& t, Handler handler)
    {
        void (connection::*f)(const boost::system::error_code&, T&, boost::tuple<Handler>)
            = &connection::handle_read_header<T, Handler>;
        boost::asio::async_read(socket_, boost::asio::buffer(inbound_header_),
            boost::bind(f, this, boost::asio::placeholders::error, boost::ref(t),
                boost::make_tuple(handler)));
    }

    template <typename T, typename Handler>
    void handle_read_header(const boost::system::error_code& e, T& t, boost::tuple<Handler> handler)
    {
        if (e)
        {
            boost::get<0>(handler)(e);
        }
        else
        {
            std::istringstream is(std::string(inbound_header_, header_length));
            std::size_t inbound_data_size = 0;
            if (!(is >> std::hex >> inbound_data_size))
            {
                boost::system::error_code error(boost::asio::error::invalid_argument);
                boost::get<0>(handler)(error);
                return;
            }

            inbound_data_.resize(inbound_data_size);
            void (connection::*f)(const boost::system::error_code&, T&, boost::tuple<Handler>)
                = &connection::handle_read_data<T, Handler>;
            boost::asio::async_read(socket_, boost::asio::buffer(inbound_data_),
                boost::bind(f, this, boost::asio::placeholders::error, boost::ref(t), handler));
        }
    }

    template <typename T, typename Handler>
    void handle_read_data(const boost::system::error_code& e, T& t, boost::tuple<Handler> handler)
    {
        if (e)
        {
            boost::get<0>(handler)(e);
        }
        else
        {
            try
            {
                std::string archive_data(&inbound_data_[0], inbound_data_.size());
                std::istringstream archive_stream(archive_data);
                boost::archive::text_iarchive archive(archive_stream);
                archive >> t;
            }
            catch (std::exception& e)
            {
                boost::system::error_code error(boost::asio::error::invalid_argument);
                boost::get<0>(handler)(error);
                return;
            }

            boost::get<0>(handler)(e);
        }
    }

private:
    boost::asio::ip::tcp::socket socket_;
    enum { header_length = 8 };
    std::string outbound_header_;
    std::string outbound_data_;
    char inbound_header_[header_length];
    std::vector<char> inbound_data_;
};

class Client : public std::enable_shared_from_this<Client>
{
public:
    Client(boost::asio::io_service& io_service, const std::string& host, const std::string& port)
        : io_service_(io_service),
          socket_(io_service),
          resolver_(io_service),
          query_(host, port)
    {
        do_connect();
    }

    void do_connect()
    {
        boost::asio::ip::tcp::resolver::iterator endpoint_iterator = resolver_.resolve(query_);
        boost::asio::async_connect(socket_, endpoint_iterator,
            boost::bind(&Client::handle_connect, shared_from_this(), boost::asio::placeholders::error));
    }

    void handle_connect(const boost::system::error_code& error)
    {
        if (!error)
        {
            std::cout << "Connected to server." << std::endl;
            start_read();
        }
        else
        {
            std::cout << "Connect failed: " << error.message() << std::endl;
            retry_connect();
        }
    }

    void start_read()
    {
        connection_.reset(new connection(socket_));
        connection_->async_read(data_,
            boost::bind(&Client::handle_read, shared_from_this(), boost::asio::placeholders::error));
    }

    void handle_read(const boost::system::error_code& error)
    {
        if (!error)
        {
            {
                std::lock_guard<std::mutex> lock(data_mutex_);
                std::cout << "Received data from server." << std::endl;
                for (const auto& d : data_)
                {
                    std::cout << "ID: " << d.id << " Content: " << d.content << std::endl;
                }
            }
            start_read();
        }
        else
        {
            std::cout << "Read failed: " << error.message() << std::endl;
            retry_connect();
        }
    }

    void retry_connect()
    {
        std::cout << "Retrying connection in 5 seconds..." << std::endl;
        boost::asio::deadline_timer timer(io_service_, boost::posix_time::seconds(5));
        timer.wait();
        do_connect();
    }

    const std::vector<Data>& get_data() const
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        return data_;
    }

private:
    boost::asio::io_service& io_service_;
    boost::asio::ip::tcp::socket socket_;
    boost::asio::ip::tcp::resolver resolver_;
    boost::asio::ip::tcp::resolver::query query_;
    boost::shared_ptr<connection> connection_;
    mutable std::mutex data_mutex_;
    std::vector<Data> data_;
};

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr << "Usage: client <host> <port>" << std::endl;
        return 1;
    }

    try
    {
        boost::asio::io_service io_service;
        std::shared_ptr<Client> client = std::make_shared<Client>(io_service, argv[1], argv[2]);

        std::thread t([&io_service]() { io_service.run(); });

        std::this_thread::sleep_for(std::chrono::seconds(10));
        const std::vector<Data>& received_data = client->get_data();
        for (const auto& d : received_data)
        {
            std::cout << "Main Thread - ID: " << d.id << " Content: " << d.content << std::endl;
        }

        t.join();
    }
    catch (std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    return 0;
}
