#include "Protocol.h"

namespace slopsmith::sandbox::wire {

juce::var makeRequest(int requestId, const char* op, const juce::var& args)
{
    juce::DynamicObject::Ptr obj(new juce::DynamicObject());
    obj->setProperty("v", static_cast<int>(kProtocolVersion));
    obj->setProperty("id", requestId);
    obj->setProperty("op", juce::String(op));
    obj->setProperty("args", args);
    return juce::var(obj.get());
}

juce::var makeEvent(const char* eventName, const juce::var& data)
{
    juce::DynamicObject::Ptr obj(new juce::DynamicObject());
    obj->setProperty("v", static_cast<int>(kProtocolVersion));
    obj->setProperty("id", juce::var());
    obj->setProperty("event", juce::String(eventName));
    obj->setProperty("data", data);
    return juce::var(obj.get());
}

juce::var makeReply(int requestId, bool ok, const juce::var& result,
                    const juce::String& errorMessage)
{
    juce::DynamicObject::Ptr obj(new juce::DynamicObject());
    obj->setProperty("v", static_cast<int>(kProtocolVersion));
    obj->setProperty("id", requestId);
    obj->setProperty("ok", ok);
    if (ok)
        obj->setProperty("result", result);
    else
        obj->setProperty("error", errorMessage);
    return juce::var(obj.get());
}

juce::MemoryBlock encode(const juce::var& v)
{
    auto json = juce::JSON::toString(v, /*allOnOneLine*/ true);
    auto utf8 = json.toUTF8();
    return juce::MemoryBlock(utf8.getAddress(), utf8.sizeInBytes() - 1);
}

juce::var decode(const void* data, size_t bytes, juce::String* errorOut)
{
    if (data == nullptr || bytes == 0)
    {
        if (errorOut) *errorOut = "empty message";
        return {};
    }
    juce::String text(juce::CharPointer_UTF8(static_cast<const char*>(data)),
                      bytes);
    juce::var parsed;
    auto result = juce::JSON::parse(text, parsed);
    if (result.failed())
    {
        if (errorOut) *errorOut = result.getErrorMessage();
        return {};
    }
    return parsed;
}

} // namespace slopsmith::sandbox::wire
