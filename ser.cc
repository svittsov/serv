#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <iostream>
#include <vector>
#include <mutex>
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

class Server
{
public:
    Server(boost::asio::io_service& io_service, short port)
        : io_service_(io_service),
          acceptor_(io_service, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port))
    {
        do_accept();
    }

    void send_data(const std::vector<Data>& data)
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& connection : connections_)
        {
            connection->async_write(data, [](const boost::system::error_code&, std::size_t){});
        }
    }

private:
    void do_accept()
    {
        auto socket = std::make_shared<boost::asio::ip::tcp::socket>(io_service_);
        acceptor_.async_accept(*socket,
            [this, socket](const boost::system::error_code& error)
            {
                if (!error)
                {
                    std::lock_guard<std::mutex> lock(clients_mutex_);
                    auto new_connection = std::make_shared<connection>(*socket);
                    connections_.emplace_back(new_connection);
                    new_connection->async_read(data_, [this, new_connection](const boost::system::error_code& error)
                    {
                        if (!error)
                        {
                            std::cout << "Received data from client." << std::endl;
                            // Do something with received data if needed
                        }
                        else
                        {
                            std::cerr << "Read failed: " << error.message() << std::endl;
                            remove_connection(new_connection);
                        }
                    });
                }
                do_accept();
            });
    }

    void remove_connection(std::shared_ptr<connection> connection)
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        connections_.erase(std::remove(connections_.begin(), connections_.end(), connection), connections_.end());
    }

    boost::asio::io_service& io_service_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::vector<std::shared_ptr<connection>> connections_;
    std::mutex clients_mutex_;
    std::vector<Data> data_;
};

int main()
{
    try
    {
        boost::asio::io_service io_service;
        Server server(io_service, 12345); // Example port 12345

        // Example sending data from outside
        std::vector<Data> data;
        // Populate data
        server.send_data(data);

        io_service.run();
    }
    catch (std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    return 0;
}
