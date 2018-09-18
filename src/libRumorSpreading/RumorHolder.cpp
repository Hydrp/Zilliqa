#include "RumorHolder.h"
#include "libUtils/Logger.h"

#include <cassert>
#include <random>

#define LITERAL(s) #s

namespace RRS
{

    // STATIC MEMBERS
    std::map<RumorHolder::StatisticKey, std::string>
        RumorHolder::s_enumKeyToString = {
            {StatisticKey::NumPeers, LITERAL(NumPeers)},
            {StatisticKey::NumMessagesReceived, LITERAL(NumMessagesReceived)},
            {StatisticKey::Rounds, LITERAL(Rounds)},
            {StatisticKey::NumPushMessages, LITERAL(NumPushMessages)},
            {StatisticKey::NumEmptyPushMessages, LITERAL(NumEmptyPushMessages)},
            {StatisticKey::NumPullMessages, LITERAL(NumPullMessages)},
            {StatisticKey::NumEmptyPullMessages, LITERAL(NumEmptyPullMessages)},
    };

    // PRIVATE METHODS
    void RumorHolder::toVector(const std::unordered_set<int>& peers)
    {
        for (const int p : peers)
        {
            if (p != m_id)
            {
                m_peers.push_back(p);
            }
        }
        increaseStatValue(StatisticKey::NumPeers, peers.size() - 1);
    }

    int RumorHolder::chooseRandomMember()
    {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dis(
            0, static_cast<int>(m_peers.size() - 1));
        return m_peers[dis(gen)];
    }

    void RumorHolder::increaseStatValue(StatisticKey key, double value)
    {
        if (m_statistics.count(key) <= 0)
        {
            m_statistics[key] = value;
        }
        else
        {
            m_statistics[key] += value;
        }
    }

    // CONSTRUCTORS
    RumorHolder::RumorHolder(const std::unordered_set<int>& peers, int id)
        : m_id(id)
        , m_networkConfig(peers.size())
        , m_peers()
        , m_rumors()
        , m_mutex()
        , m_nextMemberCb()
    {
        toVector(peers);
    }

    RumorHolder::RumorHolder(const std::unordered_set<int>& peers,
                             const NextMemberCb& cb, int id)
        : m_id(id)
        , m_networkConfig(peers.size())
        , m_peers()
        , m_rumors()
        , m_mutex()
        , m_nextMemberCb(cb)
    {
        toVector(peers);
    }

    RumorHolder::RumorHolder(const std::unordered_set<int>& peers,
                             const NetworkConfig& networkConfig, int id)
        : m_id(id)
        , m_networkConfig(networkConfig)
        , m_peers()
        , m_rumors()
        , m_mutex()
        , m_nextMemberCb()
        , m_statistics()
    {
        assert(networkConfig.networkSize() == peers.size());
        toVector(peers);
    }

    RumorHolder::RumorHolder(const std::unordered_set<int>& peers,
                             const NetworkConfig& networkConfig,
                             const NextMemberCb& cb, int id)
        : m_id(id)
        , m_networkConfig(networkConfig)
        , m_peers()
        , m_rumors()
        , m_mutex()
        , m_nextMemberCb(cb)
        , m_statistics()
    {
        assert(networkConfig.networkSize() == peers.size());
        toVector(peers);
    }

    // COPY CONSTRUCTOR
    RumorHolder::RumorHolder(const RumorHolder& other)
        : m_id(other.m_id)
        , m_networkConfig(other.m_networkConfig)
        , m_peers(other.m_peers)
        , m_rumors(other.m_rumors)
        , m_mutex()
        , m_nextMemberCb(other.m_nextMemberCb)
        , m_statistics(other.m_statistics)
    {
    }

    // MOVE CONSTRUCTOR
    RumorHolder::RumorHolder(RumorHolder&& other) noexcept
        : m_id(other.m_id)
        , m_networkConfig(other.m_networkConfig)
        , m_peers(std::move(other.m_peers))
        , m_rumors(std::move(other.m_rumors))
        , m_mutex()
        , m_nextMemberCb(std::move(other.m_nextMemberCb))
        , m_statistics(std::move(other.m_statistics))
    {
    }

    // PUBLIC METHODS
    bool RumorHolder::addRumor(int rumorId)
    {
        LOG_MARKER();
        std::lock_guard<std::mutex> guard(m_mutex); // critical section
        return m_rumors.insert(std::make_pair(rumorId, &m_networkConfig))
            .second;
    }

    std::pair<int, std::vector<Message>>
    RumorHolder::receivedMessage(const Message& message, int fromPeer)
    {
        LOG_MARKER();
        std::lock_guard<std::mutex> guard(m_mutex); // critical section

        bool isNewPeer = m_peersInCurrentRound.insert(fromPeer).second;
        increaseStatValue(StatisticKey::NumMessagesReceived, 1);

        // If this is the first time 'fromPeer' sent a PUSH/EMPTY_PUSH message in this round
        // then respond with a PULL message for each rumor
        std::vector<Message> pullMessages;
        if (isNewPeer
            && (message.type() == Message::Type::PUSH
                || message.type() == Message::Type::EMPTY_PUSH))
        {
            for (auto& kv : m_rumors)
            {
                RumorStateMachine& stateMach = kv.second;
                if (stateMach.age() > 0 and !stateMach.isOld())
                {
                    pullMessages.emplace_back(Message(
                        Message::Type::PULL, kv.first, kv.second.age()));
                }
            }

            // No PULL messages to sent i.e. no rumors received yet,
            // then send EMPTY_PULL Message. so the receiving peer will
            if (pullMessages.empty())
            {
                pullMessages.emplace_back(
                    Message(Message::Type::EMPTY_PULL, -1, 0));
                increaseStatValue(StatisticKey::NumEmptyPullMessages, 1);
            }
            else
            {
                increaseStatValue(StatisticKey::NumPullMessages,
                                  pullMessages.size());
            }
        }

        // An empty response from a peer that was sent a PULL
        const int receivedRumorId = message.rumorId();
        const int theirRound = message.age();
        if (receivedRumorId >= 0)
        {
            if (m_rumors.count(receivedRumorId) > 0)
            {
                m_rumors[receivedRumorId].rumorReceived(fromPeer,
                                                        message.age());
            }
            else
            {
                m_rumors[receivedRumorId]
                    = RumorStateMachine(&m_networkConfig, fromPeer, theirRound);
            }
        }

        return std::make_pair(fromPeer, pullMessages);
    }

    std::pair<int, std::vector<Message>> RumorHolder::advanceRound()
    {
        LOG_MARKER();
        std::lock_guard<std::mutex> guard(m_mutex); // critical section

        if (m_peers.size() <= 0)
        {
            return std::make_pair(-1, std::vector<Message>());
        }

        increaseStatValue(StatisticKey::Rounds, 1);

        int toMember = m_nextMemberCb ? m_nextMemberCb() : chooseRandomMember();

        // Construct the push messages
        std::vector<Message> pushMessages;
        for (auto& r : m_rumors)
        {
            RumorStateMachine& stateMach = r.second;
            stateMach.advanceRound(m_peersInCurrentRound);
            if (!stateMach.isOld())
            {
                pushMessages.emplace_back(
                    Message(Message::Type::PUSH, r.first, r.second.age()));
            }
        }
        increaseStatValue(StatisticKey::NumPushMessages, pushMessages.size());

        // No PUSH messages but still want to sent a response to peer.
        if (pushMessages.empty())
        {
            pushMessages.emplace_back(
                Message(Message::Type::EMPTY_PUSH, -1, 0));
            increaseStatValue(StatisticKey::NumEmptyPushMessages, 1);
        }

        // Clear round state
        m_peersInCurrentRound.clear();

        return std::make_pair(toMember, pushMessages);
    }

    // PUBLIC CONST METHODS
    int RumorHolder::id() const { return m_id; }

    const NetworkConfig& RumorHolder::networkConfig() const
    {
        return m_networkConfig;
    }

    const std::unordered_map<int, RumorStateMachine>&
    RumorHolder::rumorsMap() const
    {
        return m_rumors;
    }

    const std::map<RumorHolder::StatisticKey, double>&
    RumorHolder::statistics() const
    {
        return m_statistics;
    }

    bool RumorHolder::rumorExists(int rumorId) const
    {
        std::lock_guard<std::mutex> guard(m_mutex); // critical section
        return m_rumors.count(rumorId) > 0;
    }

    std::ostream& RumorHolder::printStatistics(std::ostream& outStream) const
    {
        outStream << m_id << ": {"
                  << "\n";
        for (const auto& stat : m_statistics)
        {
            outStream << "  " << s_enumKeyToString.at(stat.first) << ": "
                      << stat.second << "\n";
        }
        outStream << "}";
        return outStream;
    }

    // OPERATORS
    bool RumorHolder::operator==(const RumorHolder& other) const
    {
        return m_id == other.m_id;
    }

    // FREE OPERATORS
    int MemberHash::operator()(const RRS::RumorHolder& obj) const
    {
        return obj.id();
    }

} // project namespace