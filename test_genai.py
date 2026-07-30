import asyncio
from google import genai

async def main():
    print(dir(genai.client.Client().aio.live.connect))

asyncio.run(main())
