
struct Runtime;
struct ClientInputIntent;

class ClientPrediction {
public:
    void update(
        Runtime& runtime,
        const ClientInputIntent& intent,
        double deltaTime
    );
};