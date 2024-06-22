from fastapi import FastAPI

app = FastAPI()

@app.get("/")
def root():
    return "Hello from my PC"
