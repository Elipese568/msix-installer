using System.Collections.Generic;
using Msixpkg.Core.Json;

namespace Msixpkg.Core.Tests;

public class JsonTests
{
    [Fact]
    public void ParsesObjectAndWritesBackCanonically()
    {
        const string json = "{\"a\":1,\"b\":[true,false,null,\"x\"],\"c\":{\"d\":\"e\"}}";
        JsonValue value = JsonParser.Parse(json);

        Assert.Equal(JsonKind.Object, value.Kind);
        Assert.Equal(1L, value.Get("a")!.IntegerValue);
        Assert.Equal(4, value.Get("b")!.Items.Count);
        Assert.Equal(JsonKind.Null, value.Get("b")!.Items[2].Kind);
        Assert.Equal("e", value.Get("c")!.Get("d")!.StringValue);

        Assert.Equal(
            "{\n  \"a\": 1,\n  \"b\": [\n    true,\n    false,\n    null,\n    \"x\"\n  ],\n  \"c\": {\n    \"d\": \"e\"\n  }\n}\n",
            value.ToJson());
    }

    [Theory]
    [InlineData("{\"a\":1,}")]
    [InlineData("{\"a\":1}extra")]
    [InlineData("[1,2")]
    [InlineData("{\"a\":1.5}")]
    [InlineData("{\"a\":01}")]
    [InlineData("{\"a\":\"\\q\"}")]
    [InlineData("{\"a\":\"unterminated}")]
    [InlineData("{a:1}")]
    [InlineData("{\"a\":1,\"a\":2}")]
    public void RejectsInvalidJson(string json)
    {
        Assert.Throws<JsonFormatException>(() => JsonParser.Parse(json));
    }

    [Fact]
    public void RejectsUnescapedControlCharacter()
    {
        Assert.Throws<JsonFormatException>(() => JsonParser.Parse("{\"a\":\"line\nbreak\"}"));
    }

    [Fact]
    public void RejectsExcessiveNesting()
    {
        string deep = new string('[', 40) + new string(']', 40);
        Assert.Throws<JsonFormatException>(() => JsonParser.Parse(deep));
    }

    [Fact]
    public void RejectsLeadingZeroAndNegativeZero()
    {
        Assert.Throws<JsonFormatException>(() => JsonParser.Parse("{\"a\":-0}"));
        Assert.Throws<JsonFormatException>(() => JsonParser.Parse("{\"a\":007}"));
    }

    [Fact]
    public void RoundTripsAllEscapeSequences()
    {
        JsonValue value = JsonParser.Parse("{\"a\":\"q\\\"\\\\\\/\\b\\f\\n\\r\\t\\u0041\"}");
        Assert.Equal("q\"\\/\b\f\n\r\tA", value.Get("a")!.StringValue);
    }

    [Fact]
    public void RejectsDuplicateMemberOnWrite()
    {
        JsonValue value = JsonValue.NewObject();
        value.Add("a", JsonValue.Of(1L));
        Assert.Throws<InvalidOperationException>(() => value.Add("a", JsonValue.Of(2L)));
    }

    [Fact]
    public void PreservesMemberOrder()
    {
        JsonValue value = JsonParser.Parse("{\"z\":1,\"a\":2}");
        var names = new List<string>();
        foreach (KeyValuePair<string, JsonValue> member in value.Members)
        {
            names.Add(member.Key);
        }

        Assert.Equal(new[] { "z", "a" }, names);
    }
}
